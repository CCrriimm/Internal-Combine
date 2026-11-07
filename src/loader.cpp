#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include <sstream>
#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "ui_theme.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace {

constexpr const char* kBrand = "Comet";
constexpr const char* kVersion = "v2.1";
constexpr const wchar_t* kGame = L"BloodStrike.exe";
constexpr uint8_t kMagic[8] = {'B', 'S', 'L', 'D', 'P', 'A', 'Y', '1'};

const ImU32 kAccent  = CiyEUi::Accent;
const ImU32 kText    = CiyEUi::Text;
const ImU32 kMuted   = CiyEUi::Muted;
const ImU32 kSuccess = CiyEUi::Success;
const ImU32 kError   = CiyEUi::Danger;
const ImU32 kWarn    = CiyEUi::Warning;

// ---------------------------------------------------------------------------
// D3D / window state
// ---------------------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_running = true;
float g_scale = 1.0f;

ImFont* g_font = nullptr;
ImFont* g_font_bold = nullptr;
ImFont* g_font_med = nullptr;
ImFont* g_font_small = nullptr;
ImFont* g_font_mono = nullptr;

inline float S(float v) { return v * g_scale; }

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got,
                                           &g_context);
    }
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) {
        g_swap->Release();
        g_swap = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

std::mutex g_log_mutex;
std::deque<std::string> g_log_lines;
std::atomic<int> g_log_seq{0};

void Log(const std::string& msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char head[24];
    sprintf_s(head, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_lines.push_back(head + msg);
    while (g_log_lines.size() > 800) g_log_lines.pop_front();
    g_log_seq.fetch_add(1);
}

// ---------------------------------------------------------------------------
// Strings / files
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring SelfDir() {
    std::wstring p = SelfPath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (1ll << 30)) {
        CloseHandle(f);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(f);
    return ok && read == out.size();
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == data.size();
}

// ---------------------------------------------------------------------------
// Embedded payload
// ---------------------------------------------------------------------------

struct PayloadEntry {
    std::string name;
    std::vector<uint8_t> data;
};

bool LoadPayload(std::vector<PayloadEntry>& out) {
    std::vector<uint8_t> file;
    if (!ReadFileBytes(SelfPath(), file)) return false;
    if (file.size() < 16) return false;

    const uint8_t* foot = file.data() + file.size() - 16;
    if (memcmp(foot, kMagic, 8) != 0) return false;

    uint64_t payload_size = 0;
    memcpy(&payload_size, foot + 8, 8);
    if (payload_size == 0 || payload_size > file.size() - 16) return false;

    const size_t body_end = file.size() - 16;
    size_t pos = body_end - (size_t)payload_size;

    while (pos + 4 <= body_end) {
        uint32_t name_len = 0;
        memcpy(&name_len, file.data() + pos, 4);
        pos += 4;
        if (name_len == 0 || name_len > 512 || pos + name_len > body_end) return false;

        std::string name((const char*)file.data() + pos, name_len);
        pos += name_len;

        if (pos + 8 > body_end) return false;
        uint64_t data_len = 0;
        memcpy(&data_len, file.data() + pos, 8);
        pos += 8;
        if (data_len > body_end || pos + (size_t)data_len > body_end) return false;

        PayloadEntry entry;
        entry.name = std::move(name);
        entry.data.assign(file.begin() + pos, file.begin() + pos + (size_t)data_len);
        pos += (size_t)data_len;
        out.push_back(std::move(entry));
    }
    return !out.empty();
}

std::wstring MakeExtractDir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"Comet_Loader";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

void WriteRootHint(const std::wstring& base, const std::wstring& used) {
    std::vector<uint8_t> bytes((const uint8_t*)used.data(),
                               (const uint8_t*)(used.data() + used.size()));
    WriteFileBytes(base + L"\\root.txt", bytes);
}

bool ExtractPayload(const std::vector<PayloadEntry>& entries, const std::wstring& dir,
                    std::wstring& used_dir) {
    auto ensure = [](const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); };
    ensure(dir);
    ensure(dir + L"\\pyexec");
    ensure(dir + L"\\driver");
    ensure(dir + L"\\ipc");

    std::wstring target = dir;
    if (GetFileAttributesW((dir + L"\\comet_pyexec.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        HANDLE probe = CreateFileW((dir + L"\\comet_pyexec.dll").c_str(), GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            char suffix[32];
            sprintf_s(suffix, "_%lu", (unsigned long)GetCurrentProcessId());
            target = dir + Utf8ToWide(suffix);
            ensure(target);
            ensure(target + L"\\pyexec");
            ensure(target + L"\\driver");
            ensure(target + L"\\ipc");
        } else {
            CloseHandle(probe);
        }
    }

    for (const PayloadEntry& entry : entries) {
        std::wstring rel = Utf8ToWide(entry.name);
        for (wchar_t& ch : rel) {
            if (ch == L'/') ch = L'\\';
        }
        std::wstring path = target + L"\\" + rel;
        size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos) ensure(path.substr(0, slash));
        if (!WriteFileBytes(path, entry.data)) {
            Log("write failed: " + entry.name);
            return false;
        }
    }
    used_dir = target;
    return true;
}

// FULL LOADER BODY CONTINUES - process injection, UI, WinMain
// See local bs34-source for complete implementation.
// This file is being restored in parts due to transfer limits.

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    return 0;
}

}  // namespace
