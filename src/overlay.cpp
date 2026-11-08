#ifndef WIN32_LEAN_AND_MEAN

#define WIN32_LEAN_AND_MEAN

#endif

#ifndef NOMINMAX

#define NOMINMAX

#endif

#include <windows.h>

#include <d3d11.h>

#include <dxgi.h>

#include <dxgi1_2.h>

#include <cstdint>

#include <cstddef>

#include <cstdio>

#include <cstdarg>

#include <cfloat>

#include <cstring>

#include <cmath>

#include <string>

#include <vector>

#include <unordered_map>

#include <unordered_set>

#include <algorithm>

#include "imgui.h"
#include "menu_theme.h"
// capture_overlay / stream-proof removed.

#include "imgui_impl_dx11.h"

#include "imgui_impl_win32.h"

#include "MinHook.h"



#include "bs_cfg.hpp"
#include "esp_geometry.hpp"
#include "radar_geometry.hpp"
#include "esp_team.hpp"
#include "esp_armor_tier.hpp"
#include "capture_guard.hpp"
#include "capture_shield.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#else
#define _ReturnAddress() __builtin_return_address(0)
#endif
#include "protected_output.hpp"




#pragma pack(push, 1)
// Cfg struct is declared in bs_cfg.hpp (shared with Menu34).



struct Ent {

    uint64_t id;

    // UTF-8. 64 bytes is ~21 CJK characters, which is what a name needs once
    // multi-byte characters and emoji are in play; 40 truncated Korean and
    // Japanese names that read perfectly well in the game.
    char name[64];

    char gun[32];

    float head[3];

    float feet[3];

    float head_screen[2];

    float feet_screen[2];

    float distance;

    float hp;

    float max_hp;

    float armor;

    float max_armor;

    int32_t team;

    uint8_t visible;

    uint8_t is_team;

    uint8_t is_monster;

    uint8_t alive;

    uint32_t is_knocked;

    float bones[16][2]; // 16 bones in normalized screen coords [0..1]

    // Comet additions. Appended at the tail so every earlier offset in the
    // serialized record - and therefore every existing reader - is unchanged.
    uint32_t hero_id;   // replicated hero_id, 0 when unknown
    uint32_t is_dying;  // runtime DYING/downed state, published for diagnostics;
                        // the KNOCKED label still uses is_knocked above.

};

struct State {

    uint32_t magic;

    uint32_t version;

    uint32_t seq;

    uint32_t count;

    uint64_t tick_ms;

    float screen_w;

    float screen_h;

    float local_pos[3];

    float cam_pos[3];

    float cam_forward[3];

    float yaw;

    float pitch;

    float fov;

    uint8_t in_match;

    uint8_t local_valid;

    uint16_t local_hero_id;

};



struct CatHead {

    uint32_t magic;

    uint32_t version;

    uint32_t hero_count;

    uint32_t gun_count;

};



struct CatEntry {

    uint32_t a;

    uint32_t b;

    uint32_t c;

    uint32_t d;

    char name[48];

};

#pragma pack(pop)



static_assert(sizeof(Cfg) == 228, "Cfg size");

// The migrations in LoadCfg() hard-code these boundaries. Pin them so a future
// layout edit cannot silently corrupt an old config instead of failing the
// build.
static_assert(offsetof(Cfg, col_team_box) == 148, "v7 legacy prefix changed");
static_assert(offsetof(Cfg, aim_key2) == 224, "v8 layout changed");

static_assert(sizeof(Ent) == 312, "Ent size");


static_assert(sizeof(State) == 84, "State size");

static_assert(sizeof(CatHead) == 16, "CatHead size");

static_assert(sizeof(CatEntry) == 64, "CatEntry size");



static const uint32_t CFG_MAGIC = 0x42534346;

static const uint32_t STATE_MAGIC = 0x42535331;

static const uint32_t CATALOG_MAGIC = 0x42534354;

// 10: Ent::name grew from 40 to 64 bytes (UTF-8 names with emoji / CJK).
static const uint32_t VERSION = 10;



static bool ReadOverlayRootHint(std::wstring& root) {

    wchar_t tmp[MAX_PATH]{};

    if (!GetTempPathW(MAX_PATH, tmp)) return false;

    std::wstring path = std::wstring(tmp) + L"Comet_BSLoader\\root.txt";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(h, nullptr);

    if (size < 2 || size > 4096) {

        CloseHandle(h);

        return false;

    }

    std::wstring buf(size / sizeof(wchar_t), L'\0');

    DWORD read = 0;

    BOOL ok = ReadFile(h, &buf[0], size, &read, nullptr);

    CloseHandle(h);

    if (!ok || read < 2) return false;

    buf.resize(read / sizeof(wchar_t));

    if (GetFileAttributesW((buf + L"\\ipc").c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    root = buf;

    return true;

}


static const std::wstring& PathBase() {

    static std::wstring base;

    if (base.empty()) {

        HMODULE self = nullptr;

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,

                           reinterpret_cast<LPCWSTR>(&PathBase), &self);

        wchar_t dll[MAX_PATH]{};

        DWORD n = GetModuleFileNameW(self, dll, MAX_PATH);

        std::wstring dir(dll, n ? n : 0);

        size_t p = dir.find_last_of(L"\\/");

        if (p != std::wstring::npos) dir.resize(p);

        std::wstring local = dir + L"\\ipc";

        std::wstring hint;

        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) base = dir;

        else if (ReadOverlayRootHint(hint)) base = hint;

        else base = dir + L"\\..\\..";

    }

    return base;

}



static const std::wstring& IpcDir() {

    static std::wstring p;

    if (p.empty()) {

        p = PathBase() + L"\\ipc";

        CreateDirectoryW(p.c_str(), nullptr);

    }

    return p;

}



static const std::wstring& CfgPath() { static std::wstring p = IpcDir() + L"\\cfg.bin"; return p; }

static const std::wstring& SavedCfgPath() {
    static const std::wstring path = [] {
        wchar_t local[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!size || size >= 32768) return IpcDir() + L"\\settings.bin";
        std::wstring dir = std::wstring(local) + L"\\Comet";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\BloodStrike";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.bin";
    }();
    return path;
}

static const std::wstring& StatePath() { static std::wstring p = IpcDir() + L"\\state.bin"; return p; }

static const std::wstring& CatalogPath() { static std::wstring p = IpcDir() + L"\\catalog.bin"; return p; }

static const std::wstring& LogPath() { static std::wstring p = PathBase() + L"\\overlay_log.txt"; return p; }



// F_* flag enums are declared in bs_cfg.hpp.



static const ImU32 ACCENT      = IM_COL32(164, 143, 246, 255);

static const ImU32 ACCENT_SOFT = IM_COL32(164, 143, 246, 120);

static const ImU32 COL_VIS     = IM_COL32(96, 235, 155, 240);

static const ImU32 COL_HID     = IM_COL32(240, 90, 90, 235);

static const ImU32 COL_TEAM    = IM_COL32(90, 170, 255, 220);

static const ImU32 COL_MON     = IM_COL32(255, 200, 80, 235);

static const ImVec4 ACCENT4    = ImVec4(.643f, .561f, .965f, 1.f);

static const ImVec4 DIM4       = ImVec4(0.62f, 0.63f, 0.67f, 1.0f);



using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);



static HMODULE g_hmodule = nullptr;

static bool g_unloading = false;

static PresentFn g_present = nullptr;

static Present1Fn g_present1 = nullptr;
static bool g_imgui_ready = false;
static thread_local bool g_inside_present = false;
struct PresentScope {
    PresentScope() {g_inside_present=true;}
    ~PresentScope() {g_inside_present=false;}
};

static ResizeBuffersFn g_resize = nullptr;

static GetBufferFn g_get_buffer = nullptr;

static WNDPROC g_wndproc = nullptr;

static HWND g_hwnd = nullptr;
static bool g_input_attached=false;
static bool g_menu_owns_capture=false;
static UINT MenuInputCloseMessage() {
    static const UINT message=RegisterWindowMessageW(L"Comet.MenuInput.Close");
    return message;
}

#ifndef WDA_EXCLUDEFROMCAPTURE

#define WDA_EXCLUDEFROMCAPTURE 0x00000011

#endif

#ifndef WDA_NONE

#define WDA_NONE 0x00000000

#endif



static IDXGISwapChain* g_swapchain = nullptr;

static ID3D11Device* g_device = nullptr;

static ID3D11DeviceContext* g_context = nullptr;

// NOT a held reference. Holding a render-target view on buffer 0 keeps that
// back buffer un-releasable for the swapchain, so the game's own wait for the
// previous buffer can never complete: frames keep being produced (the FPS
// counter climbs, a swapchain capture still sees them) while the display stops
// advancing. This is a capability flag only - the view is opened for the
// duration of one ImGui draw and released immediately (see RenderFrame).
static bool g_rtv_live = false;

static UINT g_width = 0;

static UINT g_height = 0;

static UINT g_client_w = 0;

static UINT g_client_h = 0;

static bool g_display_dirty = false;

static bool g_reset_menu_pos = false;



static bool g_init_failed = false;



static HANDLE g_shm_state_h = nullptr;

static void*  g_shm_state_p = nullptr;

static HANDLE g_shm_cfg_h   = nullptr;

static void*  g_shm_cfg_p   = nullptr;



static void InitSharedMemory() {

    if (!g_shm_state_h) {

        g_shm_state_h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, "Local\\BS_IPC_STATE");

        if (g_shm_state_h) {

            g_shm_state_p = MapViewOfFile(g_shm_state_h, FILE_MAP_ALL_ACCESS, 0, 0, 65536);

        }

    }

    if (!g_shm_cfg_h) {

        g_shm_cfg_h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Cfg), "Local\\BS_IPC_CFG");

        if (g_shm_cfg_h) {

            g_shm_cfg_p = MapViewOfFile(g_shm_cfg_h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cfg));

        }

    }

}





Cfg g_cfg{};

static bool g_cfg_ready = false;
static bool g_cfg_writable = true;
static std::string g_cfg_error;

bool g_cfg_dirty = false;

static ULONGLONG g_last_state_poll = 0;

static ULONGLONG g_last_cfg_save = 0;

static State g_state{};

static std::vector<Ent> g_ents;

// Scratch for copying entities out of the mapped file without touching g_ents
// until the snapshot is known to be complete.
static std::vector<Ent> g_ents_scratch;

static teamtint::Labels g_team_labels;

static comet_capture::Blackout g_obs{};

// ---------------------------------------------------------------------------
// Capture shield state
//
// g_surface is the separate capture-excluded window the overlay can be drawn
// into (docs/capture-protection.md). When it is ready the game's back buffer is
// never touched, which is the only way an in-process overlay stays out of OBS
// Game Capture and Steam screenshots.
// ---------------------------------------------------------------------------
static comet_surface::Output g_surface{};


static comet_shield::CleanFrameGate g_clean_frame{};
static comet_shield::Modules g_capture_modules{};
static ULONGLONG g_capture_module_poll = 0;
static ULONGLONG g_surface_retry_at = 0;
static DWORD g_render_thread = 0;
// Address of the Present implementation MinHook patched. A swap chain slot that
// still equals it has nobody else's detour above ours.
static void* g_present_entry = nullptr;

// Frame pacing, refreshed from the Present hook and shown in Settings. Cheap
// enough to keep per frame: two counters and an exponential average.
struct PacingStat {
    double fps = 0.0;
    double overlay_ms = 0.0;
    double present_ms = 0.0;
    unsigned sync = 0;
    unsigned long long frames = 0;
};
static PacingStat g_pacing{};

static bool ShieldSurfaceReady() {
    return g_surface.state == comet_surface::State::Ready;
}

// Whether the protected window should be on screen for this frame. Hides it
// while the game is not the foreground window and while the Windows snipping
// tool / Game Bar capture UI is up: a window it cannot draw over is a window
// that leaves stale ESP on screen, and an *excluded* window in a snip is the
// black rectangle the user reported.
static bool ShieldVisible() {
    if (!ShieldSurfaceReady()) return false;
    if (!comet_surface::ShouldShow(g_hwnd)) return false;
    if (comet_surface::SystemCaptureUiActive()) return false;
    return true;
}

// A topmost overlay window only shows over a windowed / borderless game. In
// exclusive fullscreen the game owns the output, the shield window sits behind
// it, and drawing there would make the overlay invisible - so that combination
// falls back to drawing in-game instead of pretending to protect anything.
// Cached for a second: this is a swap-chain query and Present runs at frame rate.
static bool GameUsesWindowedPresentation() {
    static bool cached = true;
    static ULONGLONG cached_at = 0;
    const ULONGLONG now = GetTickCount64();
    if (cached_at && now - cached_at < 1000) return cached;
    cached_at = now ? now : 1;
    if (!g_swapchain) return cached = true;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(g_swapchain->GetDesc(&desc))) return cached = true;
    return cached = desc.Windowed != FALSE;
}

static FILETIME g_state_write{};

static ULONGLONG g_state_size = 0;

static bool g_state_seen = false;

static std::vector<CatEntry> g_heroes;

static std::vector<CatEntry> g_guns;

static std::vector<uint32_t> g_hero_ids;

static std::vector<uint32_t> g_gun_ids;

static std::vector<uint32_t> g_knife_ids;

static std::vector<uint32_t> g_weapon_ids;

static std::unordered_map<uint32_t, std::vector<int>> g_hero_skins;

static std::unordered_map<uint32_t, std::vector<int>> g_gun_skins;

static std::unordered_map<uint32_t, std::string> g_gun_label;

static std::unordered_map<uint32_t, uint32_t> g_gun_kind;

static std::unordered_map<uint32_t, uint32_t> g_gun_default;

static bool g_catalog_ready = false;

static std::vector<uint8_t> g_buffer;

static ImFont* g_font_big = nullptr;
static ImFont* g_font_esp = nullptr;



static int g_bind_slot = 0;

static bool g_bind_armed = false;

static int g_hero_sel = 0;

static int g_knife_sel = 0;

static int g_weapon_sel = 0;

static char g_search[64] = {};

static int g_tab = 0;



// Has/SetF are declared inline in bs_cfg.hpp.



static void Log(const char* fmt, ...);

// Record a menu interaction in the log. Cheap (one append per click, not per
// frame) and it is what turns "clicking crashes the game" into a last-known-action
// line in overlay_log.txt instead of a guess.
static void TraceUi(const char* label) {
    if (!label) return;
    char text[96];
    size_t n = 0;
    while (label[n] && n + 1 < sizeof(text) && !(label[n] == '#' && label[n + 1] == '#')) ++n;
    memcpy(text, label, n);
    text[n] = 0;
    Log("[ui] %s", text);
}

static void Log(const char* fmt, ...) {

    char msg[1024];

    va_list ap;

    va_start(ap, fmt);

    vsnprintf(msg, sizeof(msg), fmt, ap);

    va_end(ap);

    OutputDebugStringA(msg);

    OutputDebugStringA("\n");

    FILE* f = nullptr;

    if (_wfopen_s(&f, LogPath().c_str(), L"a") == 0 && f) {

        SYSTEMTIME st{};

        GetLocalTime(&st);

        fprintf(f, "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond, msg);

        fclose(f);

    }

}

// ---------------------------------------------------------------------------
// Fault containment.
//
// Every hook body runs on the game's render thread, inside the game's own
// Present call. An exception that escapes here is not "an overlay bug" - it is
// an unhandled exception inside the game process, which terminates it. So each
// hook body is wrapped in SEH: the frame is skipped, the fault is logged, and
// after a few faults the overlay disables itself and passes Present straight
// through. A broken overlay should cost the ESP, never the game.
static int g_fault_count = 0;
static volatile bool g_render_disabled = false;

static void NoteFault(const char* where, unsigned code) {
    if (g_fault_count < 5) Log("%s fault code=%08x (fault %d)", where, code, g_fault_count + 1);
    if (++g_fault_count >= 4) {
        if (!g_render_disabled) Log("%s: repeated faults, overlay drawing disabled", where);
        g_render_disabled = true;
    }
}

// SEH on MSVC; off Windows the same bodies are type-checked with no-op macros,
// so the g++ syntax pass and the menu preview keep working and the non-Windows
// build never swallows an exception it does not understand.
#if defined(_MSC_VER)
  #define COMET_TRY __try
  #define COMET_EXCEPT(filter) __except (filter)
#else
  #define COMET_TRY {
  #define COMET_EXCEPT(filter) }
  #define GetExceptionCode() 0u
#endif


// Defined much later, next to the rest of the render path; declared here so the
// guarded wrapper below can call them.
static bool InitOverlay(IDXGISwapChain* sc);
static void RenderFrame(IDXGISwapChain* sc);
static void LogDisplayMode(bool force);

// Note on the wrappers below: MSVC refuses __try/__except in any function that
// needs C++ object unwinding (error C2712), which rules out RAII locals and
// std::string in the same body. These two wrappers therefore keep *no*
// destructible locals - just the call and the exception code - and every caller
// keeps its own RAII scope and does its logging outside.
static unsigned RenderFrameGuarded(IDXGISwapChain* sc) {
    unsigned code = 0;
    COMET_TRY {
        if (!g_imgui_ready) InitOverlay(sc);
        RenderFrame(sc);
    }
    COMET_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        code = (unsigned)GetExceptionCode();
    }
    return code;
}






static bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {

    HANDLE h = CreateFileW(path, GENERIC_READ,

                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,

                           nullptr, OPEN_EXISTING,

                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};

    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {

        CloseHandle(h);

        return false;

    }

    out.resize(static_cast<size_t>(size.QuadPart));

    DWORD got = 0;

    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);

    CloseHandle(h);

    if (!ok) {

        out.clear();

        return false;

    }

    out.resize(got);

    return true;

}



// ---------------------------------------------------------------------------
// Mapped state file.
//
// When the runtime cannot create its shared-memory block (ctypes unavailable on
// that install) it writes the snapshot into state.bin *in place*, at a fixed
// size, instead of replacing the file. That keeps one mapping valid for the whole
// session and turns a frame's state read into a memcpy out of the view - the
// open/read/close cycle measured 3.4-5.8 ms per frame as `state_avg_ms`, which is
// most of a 60 Hz frame.
//
// A snapshot can be caught half written. The writer bumps `seq` for every frame,
// so the reader copies the header, the sequence, the entities, and then checks the
// sequence again: if it moved, the frame is dropped and the previous state is
// kept. One frame of stale ESP, never a corrupt one.
static HANDLE g_state_file = INVALID_HANDLE_VALUE;
static HANDLE g_state_mapping = nullptr;
static const uint8_t* g_state_view = nullptr;
static size_t g_state_view_size = 0;
static ULONGLONG g_state_view_at = 0;
static uint32_t g_state_view_seq = 0;

static void CloseStateView() {
    if (g_state_view) { UnmapViewOfFile(g_state_view); g_state_view = nullptr; }
    if (g_state_mapping) { CloseHandle(g_state_mapping); g_state_mapping = nullptr; }
    if (g_state_file != INVALID_HANDLE_VALUE) { CloseHandle(g_state_file); g_state_file = INVALID_HANDLE_VALUE; }
    g_state_view_size = 0;
    g_state_view_at = 0;
    g_state_view_seq = 0;
}

static bool OpenStateView() {
    const std::wstring path = StatePath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < (LONGLONG)sizeof(State)) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { CloseHandle(file); return false; }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) { CloseHandle(mapping); CloseHandle(file); return false; }
    g_state_file = file;
    g_state_mapping = mapping;
    g_state_view = reinterpret_cast<const uint8_t*>(view);
    g_state_view_size = (size_t)size.QuadPart;
    g_state_view_at = GetTickCount64();
    g_state_view_seq = reinterpret_cast<const State*>(view)->seq;
    return true;
}

// Reads one snapshot out of the view. Returns false when the file is not there,
// not mapped yet, or the snapshot was being written while it was copied.
static bool ReadMappedState() {
    if (!g_state_view) {
        if (!OpenStateView()) return false;
    }
    if (g_state_view_size < sizeof(State)) return false;
    const State* head = reinterpret_cast<const State*>(g_state_view);
    const uint32_t seq_before = head->seq;
    State snapshot = *head;
    if (snapshot.magic != STATE_MAGIC || snapshot.version != VERSION) return false;

    uint32_t count = snapshot.count;
    if (count > 128) count = 128;
    const size_t need = sizeof(State) + (size_t)count * sizeof(Ent);
    if (need > g_state_view_size) return false;

    if (count > g_ents_scratch.size()) g_ents_scratch.resize(count);
    Ent* scratch = count ? g_ents_scratch.data() : nullptr;
    if (count) memcpy(scratch, g_state_view + sizeof(State), (size_t)count * sizeof(Ent));

    if (head->seq != seq_before) return false;   // written while we copied: retry next frame

    // A file replaced underneath us (older runtime, rename) would leave the view
    // frozen; reopen when the sequence has not moved for a second.
    if (seq_before == g_state_view_seq) {
        if (GetTickCount64() - g_state_view_at > 1000) {
            CloseStateView();
            if (!OpenStateView()) return false;
            return ReadMappedState();
        }
        return false;
    }
    g_state_view_seq = seq_before;
    g_state_view_at = GetTickCount64();

    snapshot.count = count;
    g_state = snapshot;
    g_ents.resize(count);
    if (count) memcpy(g_ents.data(), scratch, (size_t)count * sizeof(Ent));
    return true;
}

static bool StateFileChanged() {

    WIN32_FILE_ATTRIBUTE_DATA fad{};

    if (!GetFileAttributesExW(StatePath().c_str(), GetFileExInfoStandard, &fad)) return false;

    ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    if (g_state_seen && size == g_state_size &&

        fad.ftLastWriteTime.dwLowDateTime == g_state_write.dwLowDateTime &&

        fad.ftLastWriteTime.dwHighDateTime == g_state_write.dwHighDateTime) {

        return false;

    }

    g_state_seen = true;

    g_state_size = size;

    g_state_write = fad.ftLastWriteTime;

    return true;

}



static bool WriteAll(const wchar_t* path, const void* data, size_t size) {

    std::wstring tmp = std::wstring(path) + L".tmp";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,

                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD wrote = 0;

    BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);

    CloseHandle(h);

    if (!ok || wrote != size) {

        DeleteFileW(tmp.c_str());

        return false;

    }

    if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {

        DeleteFileW(tmp.c_str());

        return false;

    }

    return true;

}



// Derived from the tier palette in esp_armor_tier.hpp (the anchors 50/75/100/125
// with 245 alpha), so the defaults and the tier maths share one definition.
static constexpr uint32_t kDefaultArmorTier[4] = {
    (armortier::Packed(50.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(75.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(100.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(125.0f) & 0x00FFFFFFu) | (245u << 24),
};

static void DefaultCfg() {

    ZeroMemory(&g_cfg, sizeof(g_cfg));

    g_cfg.magic = CFG_MAGIC;

    g_cfg.version = VERSION;

    g_cfg.flags = F_MENU_OPEN | F_ESP | F_ESP_BOX | F_ESP_SKELETON | F_ESP_NAME | F_ESP_DISTANCE |

                 F_ESP_HEALTH | F_ESP_ARMOR | F_ESP_HEADDOT | F_AIM_FOV_CIRCLE | F_AIM_PREDICT |

                 F_UNLOCK_EFFECTS | F_BLOCK_REPORT |

                 F_RECOIL | F_ESP_COUNT | F_ESP_OUTLINE | F_ESP_GRADIENT | F_ESP_SHADOW;

    g_cfg.aim_key = VK_RBUTTON;

    g_cfg.aim_key2 = 0; // unbound by default

    g_cfg.col_fov = IM_COL32(89,162,255,180);

    g_cfg.menu_key = VK_INSERT;

    g_cfg.aim_fov = 8.0f;

    g_cfg.aim_smooth = 0.35f;

    g_cfg.aim_max_dist = 400.0f;

    g_cfg.human_smooth = 0.5f;
    g_cfg.pad[0] = CFG_UI_TAG;
    g_cfg.pad[1] = 2;

    g_cfg.humanize = 0.0f;  // 0 = pure memory damping (pyexec); raise to add mouse counter-drag.

    g_cfg.aim_bone = 0;
    g_cfg.reserved = OPT_AIM_BONES_SET | OPT_AIM_HEAD | OPT_FOV_COLOR_SET | OPT_SHIELD_MARKER;

    g_cfg.esp_max_dist = 400;

    g_cfg.col_box = IM_COL32(96, 235, 155, 240);

    g_cfg.col_box_hidden = IM_COL32(240, 90, 90, 235);

    g_cfg.col_team = IM_COL32(90, 170, 255, 220);

    g_cfg.col_monster = IM_COL32(255, 200, 80, 235);

    g_cfg.col_skel = IM_COL32(96, 235, 155, 235);

    g_cfg.col_line = IM_COL32(89, 113, 162, 190);

    g_cfg.col_name = IM_COL32(236, 237, 241, 255);

    g_cfg.col_gun = IM_COL32(214, 216, 222, 235);

    g_cfg.col_dist = IM_COL32(200, 205, 215, 240);

    g_cfg.col_health = IM_COL32(96, 235, 155, 240);

    g_cfg.col_armor = IM_COL32(80, 180, 255, 240);

    g_cfg.col_headdot = IM_COL32(255, 255, 255, 235);

    g_cfg.col_count = IM_COL32(96, 235, 155, 255);

    g_cfg.esp_style = 0;

    g_cfg.box_thickness = 1.5f;

    g_cfg.skin_hero_item = 0;

    g_cfg.skin_gun_item = 0;

    g_cfg.skin_knife_item = 0;

    g_cfg.auto_skin = 1;

    // Armor tiers (50 grey, 75 blue, 100 purple, 125 gold). Shared with the
    // v7 -> v8 migration above. col_loot / loot_max_dist stay in the struct only
    // so the byte layout - and every migration anchor - stays put; nothing reads
    // or writes them any more.
    for (int i = 0; i < 4; ++i) {
        g_cfg.col_armor_tier[i] = kDefaultArmorTier[i];
    }

    g_cfg.opts = OPT2_CAPTURE_SHIELD;

    g_cfg.noise_strength = 0.0016f;

}



static bool LoadCfg() {
    std::vector<uint8_t> buf;
    g_cfg_error.clear();
    const DWORD attributes = GetFileAttributesW(SavedCfgPath().c_str());
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    const bool read = ReadAll((missing ? CfgPath() : SavedCfgPath()).c_str(), buf);
    // v7 -> v8 migration. The Comet tail was appended after col_armor, so the
    // first 148 bytes of a v7 file are byte-identical to the new layout and the
    // old pad[3] simply moves to the end. Without this, growing Cfg would throw
    // away every saved preference once - including the user's keybinds and
    // colours - which is not an acceptable cost for adding features.
    bool migrated = false;
    if (read && buf.size() == 224) {
        // v8 -> v9: only aim_key2 was appended. Everything else is identical.
        uint32_t old_magic = 0, old_version = 0;
        memcpy(&old_magic, buf.data(), sizeof(old_magic));
        memcpy(&old_version, buf.data() + sizeof(old_magic), sizeof(old_version));
        if (old_magic == CFG_MAGIC && old_version == 8) {
            Cfg m{};
            memcpy(&m, buf.data(), 224);
            m.version = VERSION;
            buf.assign(reinterpret_cast<const uint8_t*>(&m),
                       reinterpret_cast<const uint8_t*>(&m) + sizeof(m));
            migrated = true;
        }
    }
    if (read && buf.size() == 160) {
        uint32_t old_magic = 0, old_version = 0;
        memcpy(&old_magic, buf.data(), sizeof(old_magic));
        memcpy(&old_version, buf.data() + sizeof(old_magic), sizeof(old_version));
        if (old_magic == CFG_MAGIC && old_version == 7) {
            constexpr size_t kV7LegacyBytes = 148; // everything up to col_armor
            Cfg m{};
            memcpy(&m, buf.data(), kV7LegacyBytes);
            // v7's trailing pad[3] (UI tag / theme / reduced motion) is at 148.
            memcpy(m.pad, buf.data() + kV7LegacyBytes, sizeof(m.pad));
            // The new tail keeps the same defaults a fresh install would get, so
            // a migrated config cannot end up with colours switched off.
            for (int i = 0; i < 4; ++i) {
                m.col_armor_tier[i] = kDefaultArmorTier[i];
            }
            m.opts = OPT2_CAPTURE_SHIELD;
            m.noise_strength = 0.0016f;
            m.version = VERSION;
            buf.assign(reinterpret_cast<const uint8_t*>(&m),
                       reinterpret_cast<const uint8_t*>(&m) + sizeof(m));
            migrated = true;
        }
    }
    if (!read || buf.size() != sizeof(Cfg)) {
        DefaultCfg();
        g_cfg_ready = g_cfg_dirty = true;
        g_cfg_writable = missing;
        if (!missing) g_cfg_error = "Config read failed. Save to replace.";
        return false;
    }
    Cfg c{};
    memcpy(&c, buf.data(), sizeof(c));
    if (c.magic != CFG_MAGIC || c.version != VERSION) {
        DefaultCfg();
        g_cfg_ready = g_cfg_dirty = true;
        g_cfg_writable = missing;
        if (!missing) g_cfg_error = "Invalid config. Save to replace.";
        return false;
    }
    if (!std::isfinite(c.aim_fov) || c.aim_fov < 1.0f || c.aim_fov > 180.0f) c.aim_fov = 8.0f;

    if (!std::isfinite(c.aim_smooth) || c.aim_smooth < 0.01f || c.aim_smooth > 1.0f) c.aim_smooth = 0.35f;

    if (!std::isfinite(c.aim_max_dist) || c.aim_max_dist < 10.0f || c.aim_max_dist > 2000.0f) c.aim_max_dist = 400.0f;

    if (!std::isfinite(c.humanize) || c.humanize < 0.0f || c.humanize > 2.0f) c.humanize = 0.0f;

    if (c.esp_max_dist < 10 || c.esp_max_dist > 2000) c.esp_max_dist = 400;

    if (!std::isfinite(c.box_thickness) || c.box_thickness < 0.5f || c.box_thickness > 5.0f) c.box_thickness = 1.5f;

    if (c.col_box == 0) c.col_box = IM_COL32(96, 235, 155, 240);

    if (c.col_box_hidden == 0) c.col_box_hidden = IM_COL32(240, 90, 90, 235);

    if (c.col_team == 0) c.col_team = IM_COL32(90, 170, 255, 220);

    if (c.col_monster == 0) c.col_monster = IM_COL32(255, 200, 80, 235);

    if (c.col_skel == 0) c.col_skel = IM_COL32(96, 235, 155, 235);

    if (c.col_line == 0) c.col_line = IM_COL32(89, 113, 162, 190);

    if (c.col_name == 0) c.col_name = IM_COL32(236, 237, 241, 255);

    if (c.col_gun == 0) c.col_gun = IM_COL32(214, 216, 222, 235);

    if (c.col_dist == 0) c.col_dist = IM_COL32(200, 205, 215, 240);

    if (c.col_health == 0) c.col_health = IM_COL32(96, 235, 155, 240);

    if (c.col_armor == 0) c.col_armor = IM_COL32(80, 180, 255, 240);

    if (c.col_headdot == 0) c.col_headdot = IM_COL32(255, 255, 255, 235);

    if (c.col_count == 0) c.col_count = IM_COL32(96, 235, 155, 255);

    // Only the armor tier palette is user data now; team tint colours are
    // generated per team at runtime (src/esp_team.hpp).
    for (int i = 0; i < 4; ++i) {
        if (c.col_armor_tier[i] == 0) c.col_armor_tier[i] = kDefaultArmorTier[i];
    }
    if (!std::isfinite(c.noise_strength) || c.noise_strength < 0.0f || c.noise_strength > 0.02f) c.noise_strength = 0.0016f;

    if (c.aim_bone < 0 || c.aim_bone > 4) c.aim_bone = 0;

    if (c.aim_key < 0 || c.aim_key > 255) c.aim_key = VK_RBUTTON;

    // 0 means "no second key", which is a valid state (unlike aim_key).
    if (c.aim_key2 < 0 || c.aim_key2 > 255) c.aim_key2 = 0;

    if (c.menu_key < 0 || c.menu_key > 255) c.menu_key = VK_INSERT;

    if (!(c.reserved & OPT_AIM_BONES_SET)) {
        uint32_t bones = (c.flags & F_AIM_MULTIPOINT) || c.aim_bone == 4 ? OPT_AIM_BONE_MASK :
                         c.aim_bone == 0 ? OPT_AIM_HEAD : c.aim_bone == 1 ? OPT_AIM_NECK : OPT_AIM_BODY;
        c.reserved = (c.reserved & ~OPT_AIM_BONE_MASK) | OPT_AIM_BONES_SET | bones;
    }
    if (!(c.reserved & OPT_AIM_BONE_MASK)) c.reserved |= OPT_AIM_HEAD;
    if(!(c.reserved&OPT_FOV_COLOR_SET)) {
        c.col_fov=IM_COL32(89,162,255,180);c.reserved|=OPT_FOV_COLOR_SET;
    }
    c.flags &= ~F_AIM_MULTIPOINT;
    if (!std::isfinite(c.human_smooth) || c.human_smooth < 0.01f || c.human_smooth > 1.f) c.human_smooth = 0.5f;
    if (c.pad[0] != CFG_UI_TAG || c.pad[1] > 4) {
        c.pad[0] = CFG_UI_TAG; c.pad[1] = 2; c.pad[2] = 0;
    }
    c.pad[2] = c.pad[2] ? 1u : 0u;
    // One-time upgrade: builds before this one drew the overlay into the game's
    // back buffer only. The marker bit is in the overlay-only `reserved` word, so
    // this runs once per config instead of forcing the setting on every load: a
    // user who turns the protected output off keeps it off, and a fresh or
    // migrated config starts with it on.
    if (!(c.reserved & OPT_SHIELD_MARKER)) {
        c.reserved |= OPT_SHIELD_MARKER;
        c.opts |= OPT2_CAPTURE_SHIELD;
    }
    // Session commands must never be replayed when loading persistent preferences.
    c.flags = (c.flags & ~F_EXIT_CHEAT) | F_MENU_OPEN;
    c.apply_seq = c.apply_item = 0; c.apply_type = 0;
    g_cfg = c;
    // Loaded configs stay dirty so the next autosave rewrites them in the
    // current layout; that is also what flushes a v7 -> v8 migration to disk.
    g_cfg_ready = g_cfg_dirty = g_cfg_writable = true;
    if (migrated) Log("config migrated from layout v7 (settings preserved)");
    return true;
}

static bool SaveCfg() {
    if (!g_cfg_ready) return false;
    g_cfg.magic = CFG_MAGIC; g_cfg.version = VERSION;
    if (++g_cfg.seq == 0) ++g_cfg.seq;
    if (g_shm_cfg_p) memcpy(g_shm_cfg_p, &g_cfg, sizeof(g_cfg));
    const bool published = WriteAll(CfgPath().c_str(), &g_cfg, sizeof(g_cfg));
    if (!g_cfg_writable) return false;
    Cfg saved = g_cfg;
    saved.flags &= ~(F_MENU_OPEN | F_EXIT_CHEAT);
    saved.apply_seq = saved.apply_item = 0; saved.apply_type = 0;
    if (!WriteAll(SavedCfgPath().c_str(), &saved, sizeof(saved))) {
        g_cfg_error = "Config save failed. Check permissions.";
        return false;
    }
    if (!published) {
        g_cfg_error = "Saved; runtime update pending.";
        return false;
    }
    g_cfg_dirty = false;
    g_cfg_error.clear();
    return true;
}

bool SaveConfiguration() {
    g_cfg_dirty = true;
    g_cfg_writable = true;
    return SaveCfg();
}

const char* ConfigurationStatus() {
    if (!g_cfg_error.empty()) return g_cfg_error.c_str();
    return g_cfg_dirty ? "Pending save..." : "Saved automatically";
}

static void ApplyMenuPreferences() {
    const bool valid = g_cfg.pad[0] == CFG_UI_TAG && g_cfg.pad[1] <= 4;
    CometUi::SetTheme(valid ? g_cfg.pad[1] : 2);
    CometUi::ReducedMotion = valid && g_cfg.pad[2] != 0;
    CometUi::ApplyStyle();
}

static void ResetCfg() {

    DefaultCfg();

    g_cfg_ready = true;

    g_cfg_dirty = true;

    ApplyMenuPreferences();
    SaveConfiguration();

}



enum : uint32_t {

    OPT_AIM_TOGGLE = 1u << 0,

};



static bool OptAimToggle() {

    return (g_cfg.reserved & OPT_AIM_TOGGLE) != 0;

}



static void SetOptAimToggle(bool on) {

    if (on) g_cfg.reserved |= OPT_AIM_TOGGLE;

    else g_cfg.reserved &= ~OPT_AIM_TOGGLE;

    g_cfg_dirty = true;

}









// Keeps SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) in force while the
// Settings toggle is on. The guard re-asserts the affinity if the game clears
// it and backs off after an API failure instead of retrying every frame.
// A file next to the DLL disables capture protection for the whole session.
// Deliberately not an environment variable and not a config field: it has to
// work before the config is read and without the loader re-packaging anything,
// so a build can always be recovered when the protected window is invisible to
// screen capture.
// ---------------------------------------------------------------------------
// Capture protection, second design: a clean copy of the back buffer.
//
// The first design drew the overlay into a separate composition swap chain with
// its own window. It worked - Game Capture and Steam's F12 saw nothing - but it
// cost the user 240 fps -> 40-65: a second Present that has to be taken by DWM
// on every frame, and a topmost composition window over the game, which keeps the
// game out of independent flip. Measured against the user's own before/after:
// 240-250 fps without the internal, 40-65 with it.
//
// This design adds no window and no second Present. The overlay still draws into
// the game's back buffer, so it costs the same as any in-game overlay, and the
// capture tools are given something else to copy: before the overlay is drawn,
// the finished game frame is copied into `g_clean_tex`, and `IDXGISwapChain::
// GetBuffer` hands that texture to any caller that lives inside a known capture
// module. OBS Game Capture (graphics-hook64.dll) and Steam's screenshot
// (GameOverlayRenderer64.dll) both read the frame by calling GetBuffer on the
// game's swap chain, so they copy the game without the overlay. The game itself,
// our own code and everything else keep getting the real buffer.
static bool CaptureProtectionKilled();

static ID3D11Texture2D* g_clean_tex = nullptr;
static UINT g_clean_w = 0, g_clean_h = 0;

static bool CaptureProtectionOn() {
    return HasOpt2(OPT2_CAPTURE_SHIELD) && !CaptureProtectionKilled();
}

// Copies the game's finished frame out of the back buffer. Called just before the
// overlay is drawn into it, so what is captured is the frame as the game made it.
static void RefreshCleanCopy(ID3D11Texture2D* back) {
    if (!back || !g_device || !g_context) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    if (!td.Width || !td.Height) return;
    if (!g_clean_tex || g_clean_w != td.Width || g_clean_h != td.Height) {
        if (g_clean_tex) { g_clean_tex->Release(); g_clean_tex = nullptr; }
        D3D11_TEXTURE2D_DESC cd = td;
        cd.BindFlags = 0;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&cd, nullptr, &g_clean_tex)) || !g_clean_tex) {
            g_clean_tex = nullptr;
            return;
        }
        g_clean_w = td.Width;
        g_clean_h = td.Height;
        Log("capture clean copy %ux%u", g_clean_w, g_clean_h);
    }
    g_context->CopyResource(g_clean_tex, back);
}

static void ReleaseCleanCopy() {
    if (g_clean_tex) { g_clean_tex->Release(); g_clean_tex = nullptr; }
    g_clean_w = g_clean_h = 0;
}

// True when the caller of the hooked GetBuffer is a module that copies the
// frame. The address must be read inside HookGetBuffer itself: `_ReturnAddress()`
// reports the return address of the *current* function, so reading it in here
// would report a return address inside our own module.
static bool CallerIsCaptureModule(void* caller) {
    static thread_local void* last_caller = nullptr;
    static thread_local bool last_answer = false;
    if (!caller) return false;
    if (caller == last_caller) return last_answer;
    last_caller = caller;

    HMODULE module = nullptr;
    bool answer = false;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(caller), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; ++p) {
                if (*p == L'\\' || *p == L'/') base = p + 1;
            }
            static const wchar_t* kCaptureModules[] = {
                L"graphics-hook64.dll",     // OBS game capture
                L"graphics-hook32.dll",
                L"hook64.dll",              // older OBS
                L"obs-gameapi.dll",
                L"GameOverlayRenderer64.dll", // Steam screenshot / overlay
                L"GameOverlayRenderer.dll",
                L"nvspcap64.dll",           // NVIDIA share
                L"AMDRSServ.dll",           // AMD ReLive
                L"bdcam64.dll",             // Bandicam
                L"fraps64.dll",
                L"fraps.dll",
                L"DiscordHook64.dll",
                L"RTSSHooks64.dll",
                L"Overlay64.dll",
                L"EOSOVH-Win64-Shipping.dll",
            };
            for (const wchar_t* name : kCaptureModules) {
                if (_wcsicmp(base, name) == 0) { answer = true; break; }
            }
        }
    }
    last_answer = answer;
    return answer;
}

static HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* sc, UINT index, REFIID iid,
                                              void** out) {
    if (index == 0 && out && g_clean_tex && sc == g_swapchain && CaptureProtectionOn() &&
        CallerIsCaptureModule(_ReturnAddress())) {
        if (SUCCEEDED(g_clean_tex->QueryInterface(iid, out)) && *out) return S_OK;
    }
    return g_get_buffer(sc, index, iid, out);
}

static bool CaptureProtectionKilled() {
    static const bool killed = [] {
        const std::wstring flag = PathBase() + L"\\no_capture_guard.txt";
        return GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return killed;
}

// Names of in-process capture modules. GetModuleHandleW is a lookup in the
// loader's module list - no new library, no enumeration walk - and the answer is
// cached for two seconds because a capture tool only appears when the user
// starts a capture.
static comet_shield::Modules& CaptureModules() {
    const ULONGLONG now = GetTickCount64();
    if (g_capture_module_poll && now - g_capture_module_poll < 2000) return g_capture_modules;
    const bool first = g_capture_module_poll == 0;
    g_capture_module_poll = now ? now : 1;

    auto loaded = [](const wchar_t* name) { return GetModuleHandleW(name) != nullptr; };
    comet_shield::Modules m{};
    m.obs_graphics_hook = loaded(L"graphics-hook64.dll") || loaded(L"graphics-hook32.dll");
    m.obs_hook_dll      = loaded(L"hook64.dll") || loaded(L"obs-gameapi.dll");
    m.steam_overlay     = loaded(L"GameOverlayRenderer64.dll") || loaded(L"GameOverlayRenderer.dll");
    m.nvidia_share      = loaded(L"nvspcap64.dll");
    m.amd_relive        = loaded(L"AMDRSServ.dll");
    m.other_hook        = loaded(L"bdcam64.dll") || loaded(L"bdcam.dll") ||
                          loaded(L"fraps64.dll") || loaded(L"fraps.dll") ||
                          loaded(L"DiscordHook64.dll") ||
                          loaded(L"EOSOVH-Win64-Shipping.dll");

    if (first || m.Count() != g_capture_modules.Count()) {
        char list[160] = {};
        const int n = comet_shield::Describe(m, list, (int)sizeof(list));
        Log("capture modules: %s", n ? list : "none");
    }
    g_capture_modules = m;
    return g_capture_modules;
}

// Where our Present detour sits relative to anything else that hooked the game's
// swap chain. Reported in Settings and the log; it is the piece of information
// that says whether drawing into the back buffer can leak into a capture.
static comet_shield::Chain PresentChainPosition() {
    if (!g_swapchain || !g_present_entry) return comet_shield::Chain::Unknown;
    void** vt = *reinterpret_cast<void***>(g_swapchain);
    if (!vt) return comet_shield::Chain::Unknown;
    return comet_shield::ClassifyPresentChain(vt[8], g_present_entry);
}

// Creates, refreshes or tears down the protected output. Never disables the
// overlay: if the surface cannot be created the caller keeps drawing into the
// game exactly as before and Settings says why.
static void MaintainShieldSurface() {
    // Capture protection is a clean copy of the back buffer plus a GetBuffer
    // substitution (see RefreshCleanCopy). Nothing is allocated here: the copy is
    // created on the first frame the overlay actually draws and released when the
    // switch goes off, so a game session that never enables it pays nothing.
    if (!CaptureProtectionOn()) {
        ReleaseCleanCopy();
        return;
    }
    // The old composition window is retired. If a session still has one from an
    // older build, take it down once.
    if (g_surface.state != comet_surface::State::Off && !g_surface.bound) {
        comet_surface::Destroy(g_surface);
    }
}

static void ApplyObsProtection() {
    if (!g_hwnd) {
        comet_capture::Apply(nullptr, false, g_obs, GetTickCount64());
        return;
    }
    const comet_capture::Status before = g_obs.status;
    // Display affinity on the *game* window is retired. It hides the game from
    // Windows capture, but DWM then renders the whole game as a black rectangle in
    // a screenshot or a display capture, and it also stops a flip-model game from
    // using independent flip. Applying `false` every tick clears an exclusion left
    // behind by an older config; the protected output is what covers captures that
    // matter (it keeps the overlay out of the game's own back buffer).
    const bool want = false;
    comet_capture::Apply(g_hwnd, want, g_obs, GetTickCount64());
    if (g_obs.status != before) {
        if (g_obs.status == comet_capture::Status::Denied)
            Log("obs protection denied (win32 error %lu)", g_obs.error);
        else
            Log("obs protection %s", comet_capture::Describe(g_obs));
    }
}

static bool LoadState() {

    // 1. Try shared memory first (sub-microsecond memory copy, zero disk lag)

    if (g_shm_state_p) {

        const State* s = reinterpret_cast<const State*>(g_shm_state_p);

        if (s->magic == STATE_MAGIC && s->version == VERSION) {

            // Authoritative answer to "is the state coming from memory or from the
            // file?", which is the difference between ~0 ms and a few ms per frame.
            static bool logged_shm = false;
            if (!logged_shm) { logged_shm = true; Log("state ipc: shared memory"); }

            if (s->seq != g_state.seq) {

                g_state = *s;

                uint32_t count = s->count;

                if (count > 128) count = 128;

                g_ents.resize(count);

                if (count > 0) {

                    const Ent* src = reinterpret_cast<const Ent*>(reinterpret_cast<const uint8_t*>(g_shm_state_p) + sizeof(State));

                    memcpy(g_ents.data(), src, count * sizeof(Ent));

                }
            }

            return true;

        }

    }

    // 2. Mapped state file. The runtime writes the snapshot in place at a fixed
    // size, so this mapping stays valid and a frame costs a memcpy. Reached when
    // the shared-memory block is unavailable (ctypes missing on that install).
    {
        static bool mapped_ok = true;
        if (mapped_ok) {
            if (ReadMappedState()) return true;
            // Mapped but no new snapshot yet (or one caught mid-write): keep what
            // we have rather than paying for a file read every frame.
            if (g_state_view) return true;
            mapped_ok = false;
        }
    }

    // 3. Disk file fallback. Only reached while the shared-memory block is not
    // available. The file is rewritten on every runtime tick, so "read it when it
    // changed" means a 30-40 KB read (and an antivirus scan of it) inside Present
    // on every frame - measured at ~5 ms as `pre_avg_ms`. Nothing can be fresher
    // than the 60 Hz writer, so polling faster than that is pure cost.
    {
        static ULONGLONG last_poll = 0;
        const ULONGLONG now = GetTickCount64();
        if (last_poll && now - last_poll < 8) return true;
        last_poll = now;
    }

    {
        static bool logged_disk = false;
        if (!logged_disk) { logged_disk = true; Log("state ipc: state.bin on disk"); }
    }

    if (!StateFileChanged()) return true;

    if (!ReadAll(StatePath().c_str(), g_buffer) || g_buffer.size() < sizeof(State)) return false;

    State s{};

    memcpy(&s, g_buffer.data(), sizeof(s));

    if (s.magic != STATE_MAGIC || s.version != VERSION) return false;

    size_t max_count = (g_buffer.size() - sizeof(State)) / sizeof(Ent);

    uint32_t count = s.count;

    if (count > max_count) count = static_cast<uint32_t>(max_count);

    if (count > 128) count = 128;

    g_state = s;

    g_state.count = count;

    g_ents.resize(count);

    if (count) {

        memcpy(g_ents.data(), g_buffer.data() + sizeof(State), count * sizeof(Ent));

    }

    return true;

}



static std::string NameOf(const CatEntry& e) {

    char buf[49];

    memcpy(buf, e.name, 48);

    buf[48] = 0;

    return std::string(buf);

}



static const char* HeroNameById(uint32_t id) {

    switch (id) {

        case 101: return "Ethan";

        case 102: return "Jet";

        case 103: return "Nova";

        case 104: return "Val";

        case 105: return "Hank";

        case 106: return "Spike";

        case 107: return "E.M.";

        case 108: return "Krak";

        case 110: return "Ran";

        case 111: return "Nach";

        case 112: return "Blas";

        case 113: return "Zero";

        case 114: return "Volt";

        case 115: return "Emma";

        case 116: return "Kain";

        case 117: return "Katy";

        case 119: return "Jack";

        case 122: return "Luci";

        case 124: return "Alon";

        case 125: return "Vox";

        default: return nullptr;

    }

}



static std::string HeroLabel(uint32_t id, const CatEntry* e) {

    if (const char* n = HeroNameById(id)) return std::string(n);

    if (e) {

        char tag[5] = {};

        uint32_t d = e->d;

        tag[0] = (char)(d & 0xFF);

        tag[1] = (char)((d >> 8) & 0xFF);

        tag[2] = (char)((d >> 16) & 0xFF);

        tag[3] = (char)((d >> 24) & 0xFF);

        for (int i = 0; i < 4; ++i) {

            if (tag[i] < 32 || tag[i] > 126) tag[i] = 0;

        }

        for (int i = 3; i >= 0; --i) {

            if (tag[i] == ' ' || tag[i] == 0) tag[i] = 0;

            else break;

        }

        if (tag[0]) return std::string(tag);

    }

    char buf[32];

    snprintf(buf, sizeof(buf), "Hero %u", id);

    return std::string(buf);

}


// Hero name for an ESP chip. Unknown/missing replicated ids print nothing rather
// than "Hero 0": a target without hero data must not gain a fake label.
static std::string EnemyHeroLabel(uint32_t hero_id) {

    if (!hero_id) return std::string();

    const char* known = HeroNameById(hero_id);

    if (known) return std::string(known);

    char buf[24];

    snprintf(buf, sizeof(buf), "Hero %u", hero_id);

    return std::string(buf);

}


// Per-team tint for a raw team id. The colour is generated once per match and
// held (see teamtint::Labels), so boxes and snaplines never flicker while you
// look around. `reserved` holds the user's own enemy/teammate colours, which the
// generated palette must not reuse, so a tinted team never looks untinted.
// Returns 0 when the team is unknown; callers keep their base colour.
// The set of colours the user already uses for enemies / teammates / monsters.
// Built once per frame: TeamTint is called per target, and a fresh hash set per
// call was a heap allocation inside the present hook.
struct ReservedColours {
    uint32_t values[4];
    bool count(uint32_t colour) const {
        for (uint32_t v : values)
            if (v == colour) return true;
        return false;
    }
};

// Colours a generated team tint must never land on: the two box colours are how
// the user reads "visible" / "not visible", and the teammate and bot colours are
// how they read "not a target". A tint that collided with any of them would make
// the ESP lie.
static ReservedColours BuildReservedColours() {
    return {{g_cfg.col_box & 0x00FFFFFFu, g_cfg.col_box_hidden & 0x00FFFFFFu,
             g_cfg.col_team & 0x00FFFFFFu, g_cfg.col_monster & 0x00FFFFFFu}};
}

static ImU32 TeamTint(int team, const ReservedColours& reserved) {

    if (team <= 0) return 0;

    // Alpha comes from the user's own enemy colour so a team tint reads at the
    // same weight as every other box.

    const uint32_t alpha = g_cfg.col_box & 0xFF000000u;

    return (g_team_labels.ColourFor(team, reserved) & 0x00FFFFFFu) | alpha;

}



static std::string CurrentHeroLabel() {

    uint32_t id = g_state.local_hero_id;

    if (!id) return std::string("unknown");

    auto it = g_hero_skins.find(id);

    const CatEntry* e = nullptr;

    if (it != g_hero_skins.end() && !it->second.empty()) e = &g_heroes[(size_t)it->second[0]];

    return HeroLabel(id, e);

}



static void LoadCatalog() {

    if (!ReadAll(CatalogPath().c_str(), g_buffer) || g_buffer.size() < sizeof(CatHead)) return;

    CatHead h{};

    memcpy(&h, g_buffer.data(), sizeof(h));

    if (h.magic != CATALOG_MAGIC || h.version != VERSION) return;

    if (h.gun_count < 100) return;

    size_t need = sizeof(CatHead) + (size_t)h.hero_count * sizeof(CatEntry) + (size_t)h.gun_count * sizeof(CatEntry);

    if (g_buffer.size() < need) return;

    g_heroes.resize(h.hero_count);

    g_guns.resize(h.gun_count);

    const uint8_t* p = g_buffer.data() + sizeof(CatHead);

    if (h.hero_count) memcpy(g_heroes.data(), p, (size_t)h.hero_count * sizeof(CatEntry));

    p += (size_t)h.hero_count * sizeof(CatEntry);

    if (h.gun_count) memcpy(g_guns.data(), p, (size_t)h.gun_count * sizeof(CatEntry));



    g_hero_skins.clear();

    g_gun_skins.clear();

    g_gun_label.clear();

    g_gun_kind.clear();

    g_gun_default.clear();

    g_hero_ids.clear();

    g_gun_ids.clear();

    g_knife_ids.clear();

    g_weapon_ids.clear();



    for (size_t i = 0; i < g_heroes.size(); ++i) {

        const auto& e = g_heroes[i];

        auto& v = g_hero_skins[e.a];

        if (v.empty()) g_hero_ids.push_back(e.a);

        v.push_back((int)i);

    }

    for (size_t i = 0; i < g_guns.size(); ++i) {

        const auto& e = g_guns[i];

        auto& v = g_gun_skins[e.b];

        if (v.empty()) {

            g_gun_ids.push_back(e.b);

            g_gun_label[e.b] = NameOf(e);

            g_gun_kind[e.b] = e.d;

        }

        if (g_gun_default.find(e.b) == g_gun_default.end() && e.c == 0) {

            g_gun_default[e.b] = e.a;

        }

        v.push_back((int)i);

    }

    for (uint32_t gid : g_gun_ids) {

        if (g_gun_default.find(gid) == g_gun_default.end()) {

            auto it = g_gun_skins.find(gid);

            if (it != g_gun_skins.end() && !it->second.empty()) {

                g_gun_default[gid] = g_guns[(size_t)it->second[0]].a;

            }

        }

    }

    for (uint32_t gid : g_gun_ids) {

        uint32_t k = g_gun_kind[gid];

        if (k >= 2) g_knife_ids.push_back(gid);

        else g_weapon_ids.push_back(gid);

    }

    std::sort(g_hero_ids.begin(), g_hero_ids.end());

    std::sort(g_gun_ids.begin(), g_gun_ids.end());

    std::sort(g_knife_ids.begin(), g_knife_ids.end());

    std::sort(g_weapon_ids.begin(), g_weapon_ids.end());

    g_catalog_ready = true;

    Log("catalog loaded heroes=%u guns=%u knife_guns=%u weapon_guns=%u",

        h.hero_count, h.gun_count, (unsigned)g_knife_ids.size(), (unsigned)g_weapon_ids.size());

}



static bool ContainsNoCase(const std::string& hay, const char* needle) {

    if (!needle || !needle[0]) return true;

    size_t n = strlen(needle);

    if (n > hay.size()) return false;

    for (size_t i = 0; i + n <= hay.size(); ++i) {

        bool ok = true;

        for (size_t j = 0; j < n; ++j) {

            unsigned char a = (unsigned char)hay[i + j];

            unsigned char b = (unsigned char)needle[j];

            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');

            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');

            if (a != b) {

                ok = false;

                break;

            }

        }

        if (ok) return true;

    }

    return false;

}



static const char* KeyName(int32_t vk) {

    static char buf[64];

    if (vk == 0) return "None";

    if (vk == VK_LBUTTON) return "Mouse1";

    if (vk == VK_RBUTTON) return "Mouse2";

    if (vk == VK_MBUTTON) return "Mouse3";

    if (vk == VK_XBUTTON1) return "Mouse4";

    if (vk == VK_XBUTTON2) return "Mouse5";

    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    if(vk==VK_INSERT||vk==VK_DELETE||vk==VK_HOME||vk==VK_END||vk==VK_PRIOR||vk==VK_NEXT||
       vk==VK_LEFT||vk==VK_RIGHT||vk==VK_UP||vk==VK_DOWN||vk==VK_DIVIDE||vk==VK_NUMLOCK) sc|=1u<<24;

    if (GetKeyNameTextA((LONG)sc, buf, sizeof(buf)) > 0) return buf;

    snprintf(buf, sizeof(buf), "VK%u", (unsigned)vk);

    return buf;

}



static void BeginBind(int slot) {

    g_bind_slot = slot;

    g_bind_armed = false;

}



static SHORT RawAsyncKeyState(int vk);



static void CaptureBind(int vk) {

    if (vk <= 0 || vk > 255) return;

    if (g_bind_slot == 1) g_cfg.aim_key = vk;

    else if (g_bind_slot == 2) g_cfg.aim_key2 = vk;

    else if (g_bind_slot == 3) g_cfg.menu_key = vk;

    g_bind_slot = 0;

    g_bind_armed = false;

    g_cfg_dirty = true;

}



static void ProcessBind() {

    if (!g_bind_slot) return;

    int found = 0;

    for (int vk = 1; vk < 256; ++vk) {

        if ((RawAsyncKeyState(vk) & 0x8000) != 0) {

            found = vk;

            break;

        }

    }

    if (!found) {

        g_bind_armed = true;

        return;

    }

    if (!g_bind_armed) return;

    CaptureBind(found);

}



static bool CheckU8(const char* label, uint8_t& v) {

    bool b = v != 0;

    if (CometUi::Toggle(label, &b, ImGui::GetContentRegionAvail().x)) {

        v = b ? 1 : 0;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



static bool FlagCheck(const char* label, uint32_t flag) {

    bool b = Has(flag);

    if (CometUi::Toggle(label, &b, ImGui::GetContentRegionAvail().x)) {

        SetF(flag, b);

        return true;

    }

    return false;

}



static bool SliderI32(const char* label, int32_t& v, int32_t lo, int32_t hi) {

    int tmp = v;

    if (CometUi::Slider(label, &tmp, lo, hi, "%d")) {

        v = tmp;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



static bool SliderF32(const char* label, float& v, float lo, float hi, const char* fmt) {

    float tmp = v;

    if (CometUi::Slider(label, &tmp, lo, hi, fmt)) {

        v = tmp;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



// Inventory wire format: <III> magic/version/count, then CatEntry records.
// Entry a=type, b=hero/weapon group, c=selection ID, d=selected for auto apply.
static constexpr uint32_t SKIN_INVENTORY_MAGIC = 0x42534956;
static constexpr size_t MAX_SKIN_INVENTORY = 4096;
static std::vector<CatEntry> g_skin_inventory;
static bool g_inventory_loaded = false;
static bool g_inventory_exists = false;
static bool g_inventory_writable = true;
static std::string g_inventory_error;

static const std::wstring& SkinInventoryPath() {
    static const std::wstring path = [] {
        wchar_t local[32768]{};
        DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!count || count >= 32768) return IpcDir() + L"\\skin_inventory.bin";
        std::wstring folder = std::wstring(local) + L"\\Comet";
        CreateDirectoryW(folder.c_str(), nullptr);
        folder += L"\\BloodStrike";
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder + L"\\skin_inventory.bin";
    }();
    return path;
}

static bool SaveSkinInventory(const std::vector<CatEntry>& entries);

static void LoadSkinInventory(bool reload = false) {
    if (g_inventory_loaded && !reload) return;
    g_inventory_loaded = true;
    g_inventory_error.clear();
    if (GetFileAttributesW(SkinInventoryPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            g_inventory_exists = false;
            g_inventory_writable = true;
            g_skin_inventory.clear();
            return;
        }
    }
    g_inventory_exists = true;
    g_inventory_writable = false;
    std::vector<uint8_t> bytes;
    if (!ReadAll(SkinInventoryPath().c_str(), bytes) || bytes.size() < 12) {
        g_inventory_error = "Cannot read Inventory. Your saved file has not been changed.";
        return;
    }
    uint32_t header[3]{};
    memcpy(header, bytes.data(), sizeof(header));
    if (header[0] != SKIN_INVENTORY_MAGIC || header[1] != 1 || header[2] > MAX_SKIN_INVENTORY ||
        bytes.size() != sizeof(header) + size_t(header[2]) * sizeof(CatEntry)) {
        g_inventory_error = "Inventory format is invalid. Your saved file has not been changed.";
        return;
    }
    std::vector<CatEntry> entries(header[2]);
    if (!entries.empty()) memcpy(entries.data(), bytes.data() + sizeof(header), entries.size() * sizeof(CatEntry));
    std::unordered_map<uint64_t, bool> ids, active_slots;
    for (auto& entry : entries) {
        const uint64_t id = (uint64_t(entry.a) << 32) | entry.c;
        const uint64_t slot = (uint64_t(entry.a) << 32) | entry.b;
        if (entry.a < 1 || entry.a > 3 || !entry.b || !entry.c || entry.d > 1 || ids.count(id) ||
            (entry.d && active_slots.count(slot))) {
            g_inventory_error = "Inventory contains invalid entries. Your saved file has not been changed.";
            return;
        }
        ids[id] = true;
        if (entry.d) active_slots[slot] = true;
        entry.name[47] = 0;
    }
    const auto old_size = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const CatEntry& entry) {
        return !entry.d;
    }), entries.end());
    g_skin_inventory.swap(entries);
    g_inventory_writable = true;
    if (g_skin_inventory.size() != old_size) SaveSkinInventory(g_skin_inventory);
}

static bool SaveSkinInventory(const std::vector<CatEntry>& entries) {
    if (!g_inventory_writable) return false;
    if (entries.size() > MAX_SKIN_INVENTORY) {
        g_inventory_error = "Inventory is full. Remove a saved skin before adding another.";
        return false;
    }
    uint32_t header[] = {SKIN_INVENTORY_MAGIC, 1, static_cast<uint32_t>(entries.size())};
    std::vector<uint8_t> bytes(sizeof(header) + entries.size() * sizeof(CatEntry));
    memcpy(bytes.data(), header, sizeof(header));
    if (!entries.empty()) memcpy(bytes.data() + sizeof(header), entries.data(), entries.size() * sizeof(CatEntry));
    if (!WriteAll(SkinInventoryPath().c_str(), bytes.data(), bytes.size())) {
        g_inventory_error = "Could not save Inventory. Check folder permissions and select the skin again.";
        return false;
    }
    g_skin_inventory = entries;
    g_inventory_exists = true;
    g_inventory_error.clear();
    return true;
}

static bool RememberSkin(uint32_t type, uint32_t selection) {
    LoadSkinInventory();
    if (!g_inventory_writable) return false;
    CatEntry saved{};
    bool found = false;
    if (type == 1) {
        for (const auto& entry : g_heroes) if (entry.c == selection) {
            saved = entry; saved.b = entry.a; found = true; break;
        }
    } else if (type == 2 || type == 3) {
        for (const auto& entry : g_guns) if (entry.a == selection && (entry.d >= 2) == (type == 3)) {
            saved = entry; found = true; break;
        }
    }
    if (!found) for (const auto& entry : g_skin_inventory) if (entry.a == type && entry.c == selection) {
        saved = entry; found = true; break;
    }
    if (!found || !saved.b || !selection) {
        g_inventory_error = "This skin is not in the catalog or Inventory. Reload the catalog and try again.";
        return false;
    }
    saved.a = type; saved.c = selection; saved.d = 1;
    auto entries = g_skin_inventory;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const CatEntry& entry) {
        return !entry.d || (entry.a == type && (entry.b == saved.b || entry.c == selection));
    }), entries.end());
    // Keep one equipped skin per slot; file order preserves selection recency.
    entries.push_back(saved);
    return SaveSkinInventory(entries);
}

static void MigrateSkinInventory() {
    LoadSkinInventory();
    if (g_inventory_exists || !g_inventory_writable || !g_catalog_ready) return;
    // Preserve the three selections from configurations saved before Inventory.
    const uint32_t previous[] = {g_cfg.skin_hero_item, g_cfg.skin_gun_item, g_cfg.skin_knife_item};
    for (uint32_t type = 1; type <= 3; ++type) {
        if (previous[type - 1]) RememberSkin(type, previous[type - 1]);
    }
}


static void ApplyItem(int type, uint32_t item) {
    MigrateSkinInventory();
    if (!RememberSkin(type, item)) return;

    g_cfg.apply_type = type;

    g_cfg.apply_item = item;

    if (type == 1) g_cfg.skin_hero_item = item;

    else if (type == 2) g_cfg.skin_gun_item = item;

    else if (type == 3) g_cfg.skin_knife_item = item;

    g_cfg.apply_seq++;

    if (g_cfg.apply_seq == 0) g_cfg.apply_seq = 1;

    g_cfg_dirty = true;
    SaveCfg();

}



static bool Section(const char* title) {
    ImGui::Dummy(ImVec2(0,8));
    if (g_font_big) ImGui::PushFont(g_font_big,14);
    ImGui::TextUnformatted(title);
    if (g_font_big) ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,6));
    return true;
}

static bool BeginCard(const char* id, const ImVec2& size) {

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.082f,.106f,.145f,1));

    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(CometUi::Border));

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 10.0f));

    bool r = ImGui::BeginChild(id, ImVec2(size.x, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::PopStyleVar(3);

    ImGui::PopStyleColor(2);

    return r;

}



static void EndCard() {

    ImGui::EndChild();

}



static bool TabButton(const char* label, bool active, float width) {
    const ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(CometUi::Elevated));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(CometUi::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(active?CometUi::Text:CometUi::Muted));
    const bool pressed=ImGui::Button(label,ImVec2(width,30));
    ImGui::PopStyleColor(4);
    if(active) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x,p.y+32),ImVec2(p.x+width,p.y+34),CometUi::Accent);
    return pressed;
}

static void BindRow(const char* title, int32_t vk, int slot) {

    ImGui::TextUnformatted(title);

    ImGui::SameLine(std::max(0.f, ImGui::GetContentRegionAvail().x - 110.f));

    char buf[80];

    snprintf(buf, sizeof(buf), "%s##b%d", KeyName(vk), slot);

    if (ImGui::Button(buf, ImVec2(110.0f, 0.0f))) BeginBind(slot);

}



static ImVec4 ColF(uint32_t v) {

    return ImVec4(((v >> 0) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,

                  ((v >> 16) & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f);

}



static uint32_t PackC(const float* c) {

    auto q = [](float x) { return (uint32_t)(std::min(1.0f, std::max(0.0f, x)) * 255.0f + 0.5f); };

    return q(c[0]) | (q(c[1]) << 8) | (q(c[2]) << 16) | (q(c[3]) << 24);

}



static bool ColorRow(const char* label, uint32_t& v) {

    float c[4];

    ImVec4 f = ColF(v);

    c[0] = f.x; c[1] = f.y; c[2] = f.z; c[3] = f.w;

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(0.f, ImGui::GetContentRegionAvail().x - 28.f));
    const bool changed = ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopID();
    if (changed) {

        v = PackC(c);

        g_cfg_dirty = true;

        return true;

    }

    return false;

}





// ---------------------------------------------------------------------------
// World -> screen, computed here instead of asked for per entity.
//
// Every point the ESP draws is currently projected by the game's Python side
// (`gui.WorldToScreenPercent`), and a player with a skeleton costs 18 of those
// calls per gather. The overlay already receives the camera position, the camera
// forward vector, the field of view and the screen size, which is everything the
// projection needs - the reference implementation in the user's friend's native
// overlay reads exactly this (camera position, a 3x3 basis, fov) and computes
// `focal = 1 / tan(fov/2)`, `x = cx + (px / pz) * focal * screenH/2`,
// `y = cy - (py / pz) * focal * screenH/2`.
//
// Two things are not written down anywhere: which handedness the basis has, and
// whether the engine's fov is vertical or horizontal. So both are measured
// against the engine's own answer before anything is drawn from this. Nothing in
// this block changes what is drawn - it only logs.
// ---------------------------------------------------------------------------
struct ProjFrame {
    float pos[3];
    float right[3];
    float up[3];
    float fwd[3];
    float focal;
    float w, h;
    bool valid;
};

static void Norm3(float* v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

static void Cross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// handedness: 0 = right = up x fwd, 1 = right = fwd x up (mirrored).
static bool MakeProjFrame(ProjFrame& f, int handedness, bool fov_is_vertical) {
    f.valid = false;
    if (g_state.screen_w < 32.0f || g_state.screen_h < 32.0f) return false;
    const float fov = g_state.fov;
    if (!(fov > 1.0f && fov < 179.0f)) return false;

    f.w = g_state.screen_w;
    f.h = g_state.screen_h;
    for (int i = 0; i < 3; ++i) {
        f.pos[i] = g_state.cam_pos[i];
        f.fwd[i] = g_state.cam_forward[i];
    }
    Norm3(f.fwd);
    if (f.fwd[0] == 0.0f && f.fwd[1] == 0.0f && f.fwd[2] == 0.0f) return false;

    // The game's world is Y-up (radar projects x/z, the head offset is applied to
    // y), so the camera's roll is assumed zero and up is derived from world up.
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};
    if (handedness == 0) {
        Cross3(worldUp, f.fwd, f.right);
        Norm3(f.right);
        Cross3(f.fwd, f.right, f.up);
    } else {
        Cross3(f.fwd, worldUp, f.right);
        Norm3(f.right);
        Cross3(f.right, f.fwd, f.up);
    }
    Norm3(f.up);

    const float half = (fov_is_vertical ? f.h : f.w) * 0.5f;
    f.focal = half / std::tan(fov * 0.017453292f * 0.5f);
    f.valid = std::isfinite(f.focal) && f.focal > 1.0f;
    return f.valid;
}

// Returns false when the point is behind the camera.
static bool ProjectWorld(const ProjFrame& f, const float* world, float* out_px) {
    if (!f.valid) return false;
    const float rel[3] = {world[0] - f.pos[0], world[1] - f.pos[1], world[2] - f.pos[2]};
    const float pz = rel[0] * f.fwd[0] + rel[1] * f.fwd[1] + rel[2] * f.fwd[2];
    if (pz <= 0.01f) return false;
    const float px = rel[0] * f.right[0] + rel[1] * f.right[1] + rel[2] * f.right[2];
    const float py = rel[0] * f.up[0] + rel[1] * f.up[1] + rel[2] * f.up[2];
    out_px[0] = f.w * 0.5f + (px / pz) * f.focal;
    out_px[1] = f.h * 0.5f - (py / pz) * f.focal;
    return std::isfinite(out_px[0]) && std::isfinite(out_px[1]);
}

// Compare our projection with the engine's own, and say which convention wins.
// Runs at most every 10 s and draws nothing.
static void LogProjectionCalibration() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    if (!g_state.in_match || g_ents.empty()) return;
    if (!(g_state.fov > 1.0f && g_state.fov < 179.0f)) {
        Log("[proj] no camera fov in state (fov=%.2f)", g_state.fov);
        return;
    }

    const char* names[4] = {"upxfwd,vfov", "upxfwd,hfov", "fwdxup,vfov", "fwdxup,hfov"};
    double sum[4] = {0, 0, 0, 0};
    double worst[4] = {0, 0, 0, 0};
    int used[4] = {0, 0, 0, 0};
    ProjFrame frames[4];
    for (int v = 0; v < 4; ++v) MakeProjFrame(frames[v], v & 1, (v & 2) == 0);

    int samples = 0;
    for (const Ent& e : g_ents) {
        if (samples >= 8) break;
        // The engine's answer is a percentage of the client area.
        const bool ok_head = (e.head_screen[0] >= -0.5f && e.head_screen[0] <= 1.5f &&
                              e.head_screen[1] >= -0.5f && e.head_screen[1] <= 1.5f);
        if (!ok_head) continue;
        const float want[2] = {e.head_screen[0] * g_state.screen_w,
                               e.head_screen[1] * g_state.screen_h};
        bool counted = false;
        for (int v = 0; v < 4; ++v) {
            float got[2];
            if (!ProjectWorld(frames[v], e.head, got)) continue;
            const double d = std::sqrt((double)((got[0] - want[0]) * (got[0] - want[0]) +
                                                (got[1] - want[1]) * (got[1] - want[1])));
            sum[v] += d;
            if (d > worst[v]) worst[v] = d;
            ++used[v];
            counted = true;
        }
        if (counted) ++samples;
    }
    if (samples == 0) return;

    int best = 0;
    double best_mean = 1e30;
    for (int v = 0; v < 4; ++v) {
        if (!used[v]) continue;
        const double mean = sum[v] / used[v];
        if (mean < best_mean) { best_mean = mean; best = v; }
    }
    Log("[proj] samples=%d best=%s mean_err_px=%.2f max_err_px=%.2f fov=%.1f screen=%.0fx%.0f "
        "(mean px: %s, %s, %s, %s)",
        samples, names[best], best_mean, worst[best], g_state.fov, g_state.screen_w,
        g_state.screen_h,
        used[0] ? std::to_string(sum[0] / used[0]).c_str() : "-",
        used[1] ? std::to_string(sum[1] / used[1]).c_str() : "-",
        used[2] ? std::to_string(sum[2] / used[2]).c_str() : "-",
        used[3] ? std::to_string(sum[3] / used[3]).c_str() : "-");
}

static void DrawESP() {
    if (!Has(F_ESP) || !g_state.in_match) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 1.0f || ds.y <= 1.0f) return;
    float W = ds.x;
    float H = ds.y;

    // Read the per-frame settings once. Has() is cheap, but it is called per
    // target per element, and the branch-heavy reads were measurably worse than
    // a handful of locals.
    const uint32_t flags = g_cfg.flags;
    const uint32_t opts = g_cfg.opts;
    const bool esp_show_visible = (flags & F_ESP_VISIBLE) != 0;
    const bool shadow = (flags & F_ESP_SHADOW) != 0;
    const bool team_colors_on = (opts & OPT2_TEAM_COLORS) != 0;
    const bool team_label_on = (opts & OPT2_TEAM_LABEL) != 0;
    const bool hero_label_on = (opts & OPT2_ESP_HERO) != 0;
    const float max_dist = (float)g_cfg.esp_max_dist;

    // Team tints are generated per team, but the "do not reuse these" set only
    // changes when the user edits a colour, so build it once per frame.
    ReservedColours reserved_cols = BuildReservedColours();

    if (Has(F_ESP_COUNT)) {
        int enemies = 0, knocked = 0, bots = 0, mates = 0;
        for (const auto& ce : g_ents) {
            if (ce.distance > (float)g_cfg.esp_max_dist) continue;
            if (ce.is_team && !Has(F_ESP_TEAM)) continue;
            if (ce.is_monster) bots++;
            else if (ce.is_team) mates++;
            else if (ce.is_knocked != 0) knocked++;
            else enemies++;
        }
        struct Chip { const char* label; int value; ImU32 color; };
        Chip chips[4] = {
            { "ENEMY",   enemies, IM_COL32(240,  95,  95, 255) },
            { "KNOCKED", knocked, IM_COL32(255, 170,  45, 255) },
            { "BOT",     bots,    IM_COL32(255, 205,  95, 255) },
            { "TEAM",    mates,   IM_COL32( 95, 170, 255, 255) },
        };
        float cx = 18.0f, cy = 18.0f;
        for (const auto& c : chips) {
            if (c.value == 0 && (&c - chips) != 0) continue;  // keep ENEMY always visible
            char lv[16]; snprintf(lv, sizeof(lv), "%d", c.value);
            ImVec2 lblSz = ImGui::CalcTextSize(c.label);
            ImVec2 valSz = ImGui::CalcTextSize(lv);
            float chipW = 14.0f + lblSz.x + 10.0f + valSz.x + 14.0f;
            float chipH = 26.0f;
            ImVec2 a(cx, cy), b(cx + chipW, cy + chipH);
            dl->AddRectFilled(a, b, IM_COL32(14, 16, 22, 225), 8.0f);
            dl->AddRect(a, b, IM_COL32(45, 52, 68, 220), 8.0f, 0, 1.0f);
            dl->AddCircleFilled(ImVec2(cx + 10.0f, cy + chipH * 0.5f), 3.5f, c.color);
            dl->AddText(ImVec2(cx + 20.0f, cy + (chipH - lblSz.y) * 0.5f),
                        IM_COL32(205, 210, 225, 235), c.label);
            dl->AddText(ImVec2(cx + 20.0f + lblSz.x + 10.0f, cy + (chipH - valSz.y) * 0.5f),
                        c.color, lv);
            cx += chipW + 8.0f;
        }
    }

    for (auto it = g_ents.rbegin(); it != g_ents.rend(); ++it) {
        const auto& e = *it;
        if (e.distance > max_dist) continue;
        if (e.is_team && !(flags & F_ESP_TEAM)) continue;

        auto ok = [](const float* s) {
            return std::isfinite(s[0]) && std::isfinite(s[1]) && !(s[0] == -1.f && s[1] == -1.f);
        };
        bool head_ok = ok(e.head_screen);
        bool feet_ok = ok(e.feet_screen);
        bool on_screen = (head_ok && e.head_screen[0] >= 0 && e.head_screen[0] <= 1 && e.head_screen[1] >= 0 && e.head_screen[1] <= 1) ||
                         (feet_ok && e.feet_screen[0] >= 0 && e.feet_screen[0] <= 1 && e.feet_screen[1] >= 0 && e.feet_screen[1] <= 1);
        if (head_ok && feet_ok) {
            on_screen = std::max(e.head_screen[0],e.feet_screen[0]) >= 0 && std::min(e.head_screen[0],e.feet_screen[0]) <= 1 &&
                        std::max(e.head_screen[1],e.feet_screen[1]) >= 0 && std::min(e.head_screen[1],e.feet_screen[1]) <= 1;
        }
        if (!on_screen && (g_cfg.esp_style & ESP_OFFSCREEN_ARROW)) {
            float dx, dy;
            if (head_ok || feet_ok) {
                const float* p = head_ok ? e.head_screen : e.feet_screen;
                dx = (p[0] - .5f) * W;
                dy = (p[1] - .5f) * H;
            } else {
                float x = e.feet[0] - g_state.cam_pos[0], z = e.feet[2] - g_state.cam_pos[2];
                // Behind-camera targets use horizontal camera bearing: rear points down.
                dx = x * g_state.cam_forward[2] - z * g_state.cam_forward[0];
                dy = -(x * g_state.cam_forward[0] + z * g_state.cam_forward[2]);
            }
            esp::Point tip{}, dir{};
            if (esp::Arrow(W, H, dx, dy, tip, dir)) {
                ImU32 color = e.is_team ? g_cfg.col_team : e.is_knocked ? IM_COL32(255,160,40,245) : g_cfg.col_line;
                ImVec2 a(tip.x, tip.y), b(tip.x-dir.x*16-dir.y*7, tip.y-dir.y*16+dir.x*7),
                       c(tip.x-dir.x*16+dir.y*7, tip.y-dir.y*16-dir.x*7);
                auto* front = ImGui::GetForegroundDrawList();
                front->AddTriangleFilled(a,b,c,color);
                front->AddTriangle(a,b,c,IM_COL32(0,0,0,220),1.5f);
            }
        }
        if (!head_ok && !feet_ok) continue;

        float hx = e.head_screen[0] * W;
        float hy = e.head_screen[1] * H;
        float fx = e.feet_screen[0] * W;
        float fy = e.feet_screen[1] * H;

        bool is_knocked = (e.is_knocked != 0);
        float est_scale = is_knocked ? 600.0f : 1200.0f;
        float est = std::max(6.0f, std::min(H * 0.95f, est_scale / std::max(1.0f, e.distance)));

        if (!head_ok) {
            hx = fx;
            hy = fy - est;
        }
        if (!feet_ok) {
            fx = hx;
            fy = hy + est;
        }

        if ((hx < 0 && fx < 0) || (hx > W && fx > W)) continue;
        if (fy < hy) std::swap(hy, fy);
        // Fully above or below the viewport: nothing of this target can be seen,
        // so skip all of its chip/bar/box work. This is the common case with a
        // wide ESP distance and a camera looking at the sky.
        if (fy < 0.0f || hy > H) continue;
        float h = fy - hy;

        // Dynamic minimum box height to guarantee visibility at 150m-200m+
        if (h < 6.0f) {
            float mid = (fy + hy) * 0.5f;
            hy = mid - 3.0f;
            fy = mid + 3.0f;
            h = 6.0f;
        }

        float cx = (hx + fx) * 0.5f;
        float w = std::max(6.0f, std::min(300.0f, h * (is_knocked ? 0.70f : 0.42f)));

        bool hidden = esp_show_visible && !e.visible;
        ImU32 col = hidden ? g_cfg.col_box_hidden : g_cfg.col_box;
        ImU32 scol = hidden ? g_cfg.col_box_hidden : g_cfg.col_skel;

        if (is_knocked) {
            col = IM_COL32(255, 160, 40, 245);
            scol = col;
        } else if (e.is_team) {
            col = g_cfg.col_team;
            scol = g_cfg.col_team;
        } else if (e.is_monster) {
            col = g_cfg.col_monster;
            scol = g_cfg.col_monster;
        }

        // Per-team colour, generated once and held for the match. Knocked,
        // teammate and monster colours keep priority.
        //
        //   * visibility check on  - the box keeps the visible / hidden colour and
        //     the tint goes on the skeleton, the snapline and the team label. The
        //     whole point of the check is that the colour tells you whether the
        //     target can see you, so a tint must never overwrite it.
        //   * visibility check off - the box and the line both show the team.
        //
        // The tint is picked from colours that exclude both box colours, so it can
        // never be confused with "visible" or "hidden".
        const ImU32 tint = (team_colors_on && !is_knocked && !e.is_team && !e.is_monster &&
                            e.team > 0)
                               ? TeamTint(e.team, reserved_cols)
                               : 0u;
        const bool team_tint = tint != 0;
        if (team_tint) {
            scol = tint;
            if (!esp_show_visible) col = tint;
        }

        float th = g_cfg.box_thickness;
        ImVec2 tl(cx - w * 0.5f, hy);
        ImVec2 br(cx + w * 0.5f, fy);

        auto seg = [&](ImVec2 a, ImVec2 b, ImU32 c, float t) {
            if (shadow) dl->AddLine(a, b, IM_COL32(0, 0, 0, 170), t + 1.4f);
            dl->AddLine(a, b, c, t);
        };

        ImFont* esp_font = g_font_esp ? g_font_esp : ImGui::GetFont();
        float esp_font_sz = g_font_esp ? 18.0f : 16.0f;

        auto calc_sz = [&](const char* s) -> ImVec2 {
            if (!s || !*s) return ImVec2(0.0f, 0.0f);
            return esp_font->CalcTextSizeA(esp_font_sz, FLT_MAX, 0.0f, s);
        };

        // Two draws, not nine. The old version stamped the glyph eight times for
        // an outline; at 10 targets x 4 chips that is ~320 extra text draw calls
        // per frame for a difference nobody can see at 18 px.
        auto txt = [&](ImVec2 p, ImU32 c, const char* s) {
            if (!s || !*s) return;
            if (shadow) {
                dl->AddText(esp_font, esp_font_sz, ImVec2(p.x + 1.0f, p.y + 1.0f),
                            IM_COL32(0, 0, 0, 235), s);
            }
            dl->AddText(esp_font, esp_font_sz, p, c, s);
        };

        // Chip: dark pill behind one line of text, centered at cx. Returns advance height.
        // Sharp corners (no rounding) are cheap — rounding tessellates 4 arcs per rect
        // and 10 enemies × 3 chips each would cost ~120 extra verts per frame.
        auto chip = [&](float ccx, float cy_top, const char* s, ImU32 fg) -> float {
            if (!s || !*s) return 0.0f;
            ImVec2 sz = calc_sz(s);
            float pad_x = 6.0f, pad_y = 2.0f;
            ImVec2 a(ccx - sz.x * 0.5f - pad_x, cy_top - pad_y);
            ImVec2 b(ccx + sz.x * 0.5f + pad_x, cy_top + sz.y + pad_y);
            dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 180));
            dl->AddText(esp_font, esp_font_sz, ImVec2(ccx - sz.x * 0.5f, cy_top), fg, s);
            return sz.y + pad_y * 2.0f + 2.0f;
        };

        if (Has(F_ESP_LINE)) {
            const unsigned mode = g_cfg.esp_style & ESP_LINE_ORIGIN_MASK;
            auto origin = esp::LineOrigin(W,H,mode);
            auto target = esp::LineTarget(cx,tl.y,br.y,mode);
            ImVec2 pa(origin.x,origin.y), pb(target.x,target.y);
            auto* front = ImGui::GetForegroundDrawList();
            // Snapline alpha is its own (usually softer) setting.
            const ImU32 team_line_alpha = g_cfg.col_line & 0xFF000000u;
            const ImU32 line_col = team_tint
                ? (tint & 0x00FFFFFFu) | team_line_alpha
                : g_cfg.col_line;
            front->AddLine(pa,pb,IM_COL32(0,0,0,200),3.0f);
            front->AddLine(pa,pb,line_col,1.5f);
        }

        if (Has(F_ESP_BOX)) {
            if (Has(F_ESP_FILLED)) {
                // Subtle inner tint, bottom slightly stronger for grounding
                ImU32 fill_hi = (col & 0x00FFFFFF) | 0x14000000;
                ImU32 fill_lo = (col & 0x00FFFFFF) | 0x30000000;
                dl->AddRectFilledMultiColor(tl, br, fill_hi, fill_hi, fill_lo, fill_lo);
            }
            if (Has(F_ESP_OUTLINE)) dl->AddRect(ImVec2(tl.x - 1.0f, tl.y - 1.0f), ImVec2(br.x + 1.0f, br.y + 1.0f), IM_COL32(0, 0, 0, 215), 0.0f, 0, th + 2.0f);
            if (Has(F_ESP_CORNER)) {
                float cw = std::max(4.0f, std::min(w, h) * 0.24f);
                // Accent fill tint + thin accent-colored corners
                ImU32 hair = (col & 0x00FFFFFF) | 0x66000000;
                // Top-left
                seg(ImVec2(tl.x, tl.y), ImVec2(tl.x + cw, tl.y), col, th);
                seg(ImVec2(tl.x, tl.y), ImVec2(tl.x, tl.y + cw), col, th);
                dl->AddLine(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(tl.x + cw - 2.0f, tl.y + 2.0f), hair, 1.0f);
                // Top-right
                seg(ImVec2(br.x - cw, tl.y), ImVec2(br.x, tl.y), col, th);
                seg(ImVec2(br.x, tl.y), ImVec2(br.x, tl.y + cw), col, th);
                dl->AddLine(ImVec2(br.x - cw + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, tl.y + 2.0f), hair, 1.0f);
                // Bottom-left
                seg(ImVec2(tl.x, br.y - cw), ImVec2(tl.x, br.y), col, th);
                seg(ImVec2(tl.x, br.y), ImVec2(tl.x + cw, br.y), col, th);
                dl->AddLine(ImVec2(tl.x + 2.0f, br.y - 2.0f), ImVec2(tl.x + cw - 2.0f, br.y - 2.0f), hair, 1.0f);
                // Bottom-right
                seg(ImVec2(br.x - cw, br.y), ImVec2(br.x, br.y), col, th);
                seg(ImVec2(br.x, br.y - cw), ImVec2(br.x, br.y), col, th);
                dl->AddLine(ImVec2(br.x - cw + 2.0f, br.y - 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f), hair, 1.0f);
            } else {
                dl->AddRect(tl, br, col, 1.5f, 0, th);
            }
        }

        if (Has(F_ESP_HEADDOT) && head_ok && !is_knocked) {
            // Crosshair-style: outer ring + tiny inner dot + 4 tick marks
            ImU32 dot = g_cfg.col_headdot;
            if (shadow) dl->AddCircle(ImVec2(hx, hy), 5.0f, IM_COL32(0, 0, 0, 180), 16, 1.8f);
            dl->AddCircle(ImVec2(hx, hy), 4.0f, dot, 16, 1.4f);
            dl->AddCircleFilled(ImVec2(hx, hy), 1.3f, dot);
            dl->AddLine(ImVec2(hx - 7.0f, hy), ImVec2(hx - 5.0f, hy), dot, 1.0f);
            dl->AddLine(ImVec2(hx + 5.0f, hy), ImVec2(hx + 7.0f, hy), dot, 1.0f);
            dl->AddLine(ImVec2(hx, hy - 7.0f), ImVec2(hx, hy - 5.0f), dot, 1.0f);
            dl->AddLine(ImVec2(hx, hy + 5.0f), ImVec2(hx, hy + 7.0f), dot, 1.0f);
        }

        if (Has(F_ESP_SKELETON) && !is_knocked) {
            bool has_bones = false;
            for (int bi = 0; bi < 16; ++bi) {
                if (e.bones[bi][0] >= 0.0f && e.bones[bi][1] >= 0.0f) {
                    has_bones = true;
                    break;
                }
            }
            if (has_bones) {
                static const int PAIRS[15][2] = {
                    {0, 1}, {1, 2}, {2, 3},
                    {1, 4}, {4, 5}, {5, 6},
                    {1, 7}, {7, 8}, {8, 9},
                    {3, 10}, {10, 11}, {11, 12},
                    {3, 13}, {13, 14}, {14, 15}
                };
                for (int p = 0; p < 15; ++p) {
                    const float* b1 = e.bones[PAIRS[p][0]];
                    const float* b2 = e.bones[PAIRS[p][1]];
                    if (b1[0] >= 0.0f && b1[1] >= 0.0f && b2[0] >= 0.0f && b2[1] >= 0.0f) {
                        seg(ImVec2(b1[0] * W, b1[1] * H),
                            ImVec2(b2[0] * W, b2[1] * H),
                            scol, 1.8f);
                    }
                }
                // Joint dots make the skeleton read as anatomy, not just lines.
                // 6-segment circles instead of 8 → 2 fewer tris per joint × 16 joints.
                ImU32 joint = (scol & 0x00FFFFFF) | 0xFF000000;
                for (int bi = 0; bi < 16; ++bi) {
                    const float* b = e.bones[bi];
                    if (b[0] < 0.0f || b[1] < 0.0f) continue;
                    ImVec2 jp(b[0] * W, b[1] * H);
                    dl->AddCircleFilled(jp, 1.6f, joint, 6);
                }
            } else if (h >= 14.0f) {
                float sy = hy + h * 0.28f;
                float sx = w * 0.35f;
                seg(ImVec2(hx, hy), ImVec2(hx, sy), scol, 1.3f);
                seg(ImVec2(hx - sx, sy), ImVec2(hx + sx, sy), scol, 1.3f);
                seg(ImVec2(hx, sy), ImVec2(fx, fy), scol, 1.3f);
            }
        }

        float y = hy - (esp_font_sz + 6.0f);
        if (is_knocked) {
            y -= chip(cx, y, "[KNOCKED]", IM_COL32(255, 175, 45, 255));
        }
        if (team_label_on && !e.is_monster && e.team > 0) {
            const int number = g_team_labels.NumberFor(e.team);
            if (number > 0) {
                char tbuf[24];
                snprintf(tbuf, sizeof(tbuf), "Team %d", number);
                // Same colour as the team's box/line, so the label and the target
                // are obviously the same team. Falls back to the teammate colour
                // when tints are off.
                y -= chip(cx, y, tbuf, team_tint ? tint : g_cfg.col_team);
            }
        }
        // The header above the box carries at most the knock state, the team and
        // the player name: stacking weapon + hero + name there buried the target.
        if (Has(F_ESP_NAME) && e.name[0]) {
            y -= chip(cx, y, e.name, g_cfg.col_name);
        }
        if (Has(F_ESP_GUN) && e.gun[0]) {
            y -= chip(cx, y, e.gun, g_cfg.col_gun);
        }
        // Feet row, top to bottom: distance, then hero name.
        float foot_y = fy + 6.0f;
        if (Has(F_ESP_DISTANCE)) {
            char dbuf[24];
            snprintf(dbuf, sizeof(dbuf), "%.0f m", e.distance);
            foot_y += chip(cx, foot_y, dbuf, g_cfg.col_dist);
        }
        if (hero_label_on && !e.is_team) {
            const std::string hero = EnemyHeroLabel(e.hero_id);
            if (!hero.empty()) foot_y += chip(cx, foot_y, hero.c_str(), g_cfg.col_name);
        }

        bool has_hp = Has(F_ESP_HEALTH) && e.max_hp > 0.0f;
        bool has_armor = Has(F_ESP_ARMOR) && e.max_armor > 0.0f && e.armor > 0.0f;

        // Bars live in their own stacked slot on the left of the box.
        const float bar_w = 4.0f;
        const float bar_gap = 2.0f;
        float slot_count = (has_hp ? 1.0f : 0.0f) + (has_armor ? 1.0f : 0.0f);
        float slot_right = tl.x - 4.0f;
        float first_x = slot_right - (slot_count * bar_w + (slot_count > 0 ? (slot_count - 1.0f) * bar_gap : 0.0f));

        auto draw_bar = [&](float bx, float ratio, ImU32 fg, bool is_armor) {
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            // Outlined track
            dl->AddRectFilled(ImVec2(bx - 1.0f, hy - 1.0f), ImVec2(bx + bar_w + 1.0f, fy + 1.0f),
                              IM_COL32(0, 0, 0, 220), 2.0f);
            dl->AddRectFilled(ImVec2(bx, hy), ImVec2(bx + bar_w, fy), IM_COL32(28, 32, 42, 255), 1.5f);
            float top = fy - h * ratio;
            if (Has(F_ESP_GRADIENT) && !is_knocked) {
                ImU32 low = (fg & 0x00FFFFFF) | 0x90000000;
                dl->AddRectFilledMultiColor(ImVec2(bx, top), ImVec2(bx + bar_w, fy), fg, fg, low, low);
            } else {
                dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + bar_w, fy), fg, 1.5f);
            }
            // Tick marks at 25 / 50 / 75 %
            ImU32 tick = IM_COL32(0, 0, 0, 150);
            for (int i = 1; i < 4; ++i) {
                float ty = hy + h * (i / 4.0f);
                dl->AddLine(ImVec2(bx, ty), ImVec2(bx + bar_w, ty), tick, 1.0f);
            }
            // Numeric readout for HP when the box is tall enough and HP is low (<=60%).
            // Sharp chip + skip readout entirely for distant/tiny targets.
            if (!is_armor && !is_knocked && h >= 36.0f && ratio <= 0.60f) {
                char hbuf[16];
                int cur = (int)std::round(e.hp);
                snprintf(hbuf, sizeof(hbuf), "%d", cur);
                ImVec2 sz = ImGui::CalcTextSize(hbuf);
                float ty = std::max(hy, std::min(fy - sz.y, top - sz.y * 0.5f));
                float tx = bx - sz.x - 4.0f;
                dl->AddRectFilled(ImVec2(tx - 3.0f, ty - 1.0f),
                                  ImVec2(tx + sz.x + 3.0f, ty + sz.y + 1.0f),
                                  IM_COL32(10, 12, 18, 200));
                dl->AddText(ImVec2(tx, ty), fg, hbuf);
            }
        };

        if (has_hp) {
            float ratio = e.hp / e.max_hp;
            ImU32 hp_color = is_knocked ? IM_COL32(255, 140, 0, 245) :
                             (ratio > 0.60f ? g_cfg.col_health :
                             (ratio > 0.25f ? IM_COL32(245, 215, 60, 245) : IM_COL32(245, 60, 60, 245)));
            draw_bar(first_x, ratio, hp_color, false);
        }
        if (has_armor) {
            float ratio = e.armor / e.max_armor;
            // The bar is coloured by the target's maximum armor tier, not by a
            // number next to it: 50 grey, 75 blue, 100 purple, 125 gold. This is
            // automatic whenever the armor bar is on - there is nothing to turn
            // on and nothing to type.
            const ImU32 arm_color = is_knocked
                ? IM_COL32(120, 140, 180, 240)
                : armortier::Packed(e.max_armor);
            float bx = has_hp ? (first_x + bar_w + bar_gap) : first_x;
            draw_bar(bx, ratio, arm_color, true);
        }
    }
}

static void DrawFOV() {

    if (!g_state.in_match) return;

    bool draw_aim = Has(F_AIM_FOV_CIRCLE) && Has(F_AIM);

    if (!draw_aim) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImVec2 ds = ImGui::GetIO().DisplaySize;

    if (ds.x <= 1.0f || ds.y <= 1.0f) return;

    float W2 = ds.x * 0.5f;

    float H2 = ds.y * 0.5f;

    const float game_fov=g_state.fov>1.f?g_state.fov:40.f;
    const float tan_half=std::max(.01f,std::tan(game_fov*.5f*3.14159265f/180.f));
    const float r_aim=std::max(6.f,std::tan(std::max(1.f,g_cfg.aim_fov)*.5f*3.14159265f/180.f)/tan_half*H2);
    if(r_aim<ds.y*1.5f) dl->AddCircle(ImVec2(W2,H2),r_aim,g_cfg.col_fov,64,1.5f);

    dl->AddCircleFilled(ImVec2(W2, H2), 2.5f, IM_COL32(255, 255, 255, 220), 8);

}



static void DrawRadar() {
    if (!Has(F_ESP) || !(g_cfg.reserved & OPT_ESP_RADAR) ||
        !g_state.in_match || !g_state.local_valid) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 260.f || display.y < 260.f) return;
    constexpr float radius = 92.f, range = 100.f;
    const ImVec2 center(display.x - radius - 24.f, radius + 46.f);
    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddCircleFilled(center, radius + 10.f, IM_COL32(13,18,26,220), 64);
    dl->AddCircle(center, radius, IM_COL32(90,170,210,160), 64, 1.5f);
    dl->AddCircle(center, radius * .5f, IM_COL32(95,125,150,65), 48);
    dl->AddLine(ImVec2(center.x-radius,center.y), ImVec2(center.x+radius,center.y), IM_COL32(95,125,150,65));
    dl->AddLine(ImVec2(center.x,center.y-radius), ImVec2(center.x,center.y+radius), IM_COL32(95,125,150,65));
    dl->AddText(ImVec2(center.x-radius,center.y-radius-29.f), IM_COL32(210,225,240,240), "RADAR / 100 m");
    for (const auto& e : g_ents) {
        if (!e.alive || !std::isfinite(e.distance) || e.distance > g_cfg.esp_max_dist ||
            (e.is_team && !Has(F_ESP_TEAM))) continue;
        float x, y;
        if (!radar::Project(e.feet[0]-g_state.local_pos[0], e.feet[2]-g_state.local_pos[2],
                            g_state.cam_forward[0], g_state.cam_forward[2], range, x, y)) continue;
        const ImVec2 point(center.x+x*(radius-5.f), center.y+y*(radius-5.f));
        const ImU32 color = e.is_team ? g_cfg.col_team : e.is_knocked ?
            IM_COL32(255,175,55,255) : IM_COL32(255,90,100,255);
        dl->AddCircleFilled(point, 4.f, color, 12);
        dl->AddCircle(point, 4.f, IM_COL32(0,0,0,200), 12);
    }
    dl->AddTriangleFilled(ImVec2(center.x,center.y-7.f), ImVec2(center.x-5.f,center.y+5.f),
                          ImVec2(center.x+5.f,center.y+5.f), IM_COL32(225,245,255,255));
}

static void DrawESPTab() {
    if (!ImGui::BeginTable("visuals_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("visuals_elements",ImVec2(0,0));
    Section("Player overlay");
    FlagCheck("Enable ESP",F_ESP);
    bool radar_on = (g_cfg.reserved & OPT_ESP_RADAR) != 0;
    if (CometUi::Toggle("2D radar (100 m)",&radar_on,ImGui::GetContentRegionAvail().x)) {
        g_cfg.reserved = radar_on ? g_cfg.reserved | OPT_ESP_RADAR : g_cfg.reserved & ~OPT_ESP_RADAR;
        g_cfg_dirty = true;
    }
    ImGui::Dummy(ImVec2(0,8));
    FlagCheck("Bounding box",F_ESP_BOX);
    FlagCheck("Skeleton",F_ESP_SKELETON);
    FlagCheck("Snapline",F_ESP_LINE);
    int line_origin = int(g_cfg.esp_style & ESP_LINE_ORIGIN_MASK);
    if (line_origin > 2) line_origin = 0;
    ImGui::TextUnformatted("Line origin");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##line_origin",&line_origin,"Bottom\0Top\0Mid\0")) {
        g_cfg.esp_style = (g_cfg.esp_style & ~ESP_LINE_ORIGIN_MASK) | uint32_t(line_origin);
        g_cfg_dirty = true;
    }
    bool arrows = (g_cfg.esp_style & ESP_OFFSCREEN_ARROW) != 0;
    if (CometUi::Toggle("Offscreen arrows",&arrows,ImGui::GetContentRegionAvail().x)) {
        g_cfg.esp_style = arrows ? g_cfg.esp_style | ESP_OFFSCREEN_ARROW : g_cfg.esp_style & ~ESP_OFFSCREEN_ARROW;
        g_cfg_dirty = true;
    }
    FlagCheck("Player name",F_ESP_NAME);
    FlagCheck("Weapon name",F_ESP_GUN);
    FlagCheck("Distance",F_ESP_DISTANCE);
    FlagCheck("Health bar",F_ESP_HEALTH);
    FlagCheck("Armor bar",F_ESP_ARMOR);
    FlagCheck("Head marker",F_ESP_HEADDOT);
    FlagCheck("Enemy counter",F_ESP_COUNT);
    bool hero = HasOpt2(OPT2_ESP_HERO);
    if (CometUi::Toggle("Enemy hero name",&hero,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_ESP_HERO,hero);
    bool team_label = HasOpt2(OPT2_TEAM_LABEL);
    if (CometUi::Toggle("Team label",&team_label,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_TEAM_LABEL,team_label);
    bool team_colors = HasOpt2(OPT2_TEAM_COLORS);
    if (CometUi::Toggle("Team box + line colors",&team_colors,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_TEAM_COLORS,team_colors);
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("visuals_filters",ImVec2(0,0));
    Section("Target filters");
    FlagCheck("Show teammates",F_ESP_TEAM);
    FlagCheck("Visibility check",F_ESP_VISIBLE);
    SliderI32("Maximum distance",g_cfg.esp_max_dist,50,2000);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("visuals_style",ImVec2(0,0));
    Section("Overlay style");
    FlagCheck("Corner boxes",F_ESP_CORNER);
    FlagCheck("Box outline",F_ESP_OUTLINE);
    FlagCheck("Box fill",F_ESP_FILLED);
    FlagCheck("Health gradient",F_ESP_GRADIENT);
    FlagCheck("Text shadow",F_ESP_SHADOW);
    SliderF32("Line thickness",g_cfg.box_thickness,0.5f,5.f,"%.1f px");
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("visuals_colors",ImVec2(0,0));
    if (ImGui::CollapsingHeader("Overlay colors")) {
        ColorRow("Box visible",g_cfg.col_box); ColorRow("Box hidden",g_cfg.col_box_hidden);
        ColorRow("Skeleton",g_cfg.col_skel); ColorRow("Snapline",g_cfg.col_line);
        ColorRow("Player name",g_cfg.col_name); ColorRow("Weapon name",g_cfg.col_gun);
        ColorRow("Distance",g_cfg.col_dist); ColorRow("Health",g_cfg.col_health);
        ColorRow("Head marker",g_cfg.col_headdot);
        ColorRow("Teammates",g_cfg.col_team); ColorRow("Bots",g_cfg.col_monster);
        ColorRow("Counter",g_cfg.col_count);
    if (ImGui::Button("Reset colors", ImVec2(150, 26))) {

        g_cfg.col_box = IM_COL32(96, 235, 155, 240);

        g_cfg.col_box_hidden = IM_COL32(240, 90, 90, 235);

        g_cfg.col_team = IM_COL32(90, 170, 255, 220);

        g_cfg.col_monster = IM_COL32(255, 200, 80, 235);

        g_cfg.col_skel = IM_COL32(96, 235, 155, 235);

        g_cfg.col_line = IM_COL32(89, 113, 162, 190);

        g_cfg.col_name = IM_COL32(236, 237, 241, 255);

        g_cfg.col_gun = IM_COL32(214, 216, 222, 235);

        g_cfg.col_dist = IM_COL32(200, 205, 215, 240);

        g_cfg.col_health = IM_COL32(96, 235, 155, 240);

        g_cfg.col_headdot = IM_COL32(255, 255, 255, 235);

        g_cfg.col_count = IM_COL32(96, 235, 155, 255);

        g_cfg_dirty = true;

    }

    }
    EndCard();
    ImGui::EndTable();
}

static void DrawAimTab() {
    if (!ImGui::BeginTable("aim_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("aim_targeting",ImVec2(0,0));
    Section("Targeting");
    FlagCheck("Enable aim",F_AIM);
    int aim_mode=(g_cfg.reserved&OPT_AIM_SILENT)?1:0;
    ImGui::TextUnformatted("Aim mode");ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##aim_mode",&aim_mode,"Aimbot\0Silent Aim\0")) {
        g_cfg.reserved=aim_mode?g_cfg.reserved|OPT_AIM_SILENT:g_cfg.reserved&~OPT_AIM_SILENT;
        g_cfg_dirty=true;
    }
    ImGui::Dummy(ImVec2(0,8));
    SliderF32("Field of view",g_cfg.aim_fov,1.f,180.f,"%.1f deg");
    SliderF32("Maximum distance",g_cfg.aim_max_dist,10.f,2000.f,"%.0f m");
    const char* bone_names[]={"Head","Neck","Body"};
    const uint32_t bone_flags[]={OPT_AIM_HEAD,OPT_AIM_NECK,OPT_AIM_BODY};
    uint32_t bone_mask=g_cfg.reserved&OPT_AIM_BONE_MASK;
    if(!bone_mask) bone_mask=OPT_AIM_HEAD;
    std::string selected;
    for(int i=0;i<3;++i) if(bone_mask&bone_flags[i]) {
        if(!selected.empty()) selected+=" + "; selected+=bone_names[i];
    }
    ImGui::TextUnformatted("Aim points");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##aim_points",selected.c_str())) {
        for(int i=0;i<3;++i) {
            const bool on=(bone_mask&bone_flags[i])!=0;
            if(ImGui::Selectable(bone_names[i],on,ImGuiSelectableFlags_NoAutoClosePopups)) {
                const uint32_t next=bone_mask^bone_flags[i];
                if(next) {
                    bone_mask=next;
                    g_cfg.reserved=(g_cfg.reserved&~OPT_AIM_BONE_MASK)|OPT_AIM_BONES_SET|next;
                    g_cfg.flags&=~F_AIM_MULTIPOINT; g_cfg_dirty=true;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0,8));
    FlagCheck("Visible targets only",F_AIM_VISIBLE);
    FlagCheck("Allow teammates",F_AIM_TEAM);
    bool skip=(g_cfg.reserved&OPT_AIM_IGNORE_KNOCKED)!=0;
    if(CometUi::Toggle("Ignore knocked targets",&skip,ImGui::GetContentRegionAvail().x)) {
        if(skip) g_cfg.reserved|=OPT_AIM_IGNORE_KNOCKED; else g_cfg.reserved&=~OPT_AIM_IGNORE_KNOCKED;
        g_cfg_dirty=true;
    }
    FlagCheck("Show FOV circle",F_AIM_FOV_CIRCLE);
    ColorRow("FOV color",g_cfg.col_fov);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_activation",ImVec2(0,0));
    Section("Activation");
    BindRow("Aim key",g_cfg.aim_key,1);
    // Optional second key: either one activates the aimbot, so a user can keep
    // the mouse bind and add a keyboard bind (or the other way round).
    BindRow("Aim key 2",g_cfg.aim_key2,2);
    if (g_cfg.aim_key2 != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##aim2")) { g_cfg.aim_key2 = 0; g_cfg_dirty = true; }
    } else {
        ImGui::TextDisabled("Unbound - either key activates the aimbot.");
    }
    bool toggle=OptAimToggle();
    if(CometUi::Toggle("Toggle activation",&toggle,ImGui::GetContentRegionAvail().x)) SetOptAimToggle(toggle);
    if(g_bind_slot==1||g_bind_slot==2) ImGui::TextDisabled("Press a key to assign...");
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("aim_tracking",ImVec2(0,0));
    const bool silent=(g_cfg.reserved&OPT_AIM_SILENT)!=0;
    Section(silent?"Shot targeting":"Tracking & motion");
    if(silent) ImGui::TextWrapped("Shot direction only. Camera stays unchanged.");
    else {
    SliderF32("Tracking smooth",g_cfg.aim_smooth,0.01f,1.f,"%.2f");
    FlagCheck("Humanize path",F_AIM_HUMANIZE);
    ImGui::BeginDisabled(!Has(F_AIM_HUMANIZE));
    SliderF32("Human smoothing",g_cfg.human_smooth,0.01f,1.f,"%.2f");
    ImGui::EndDisabled();
    }
    FlagCheck("Movement prediction",F_AIM_PREDICT);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_input",ImVec2(0,0));
    Section("Input");
    // Input noise is applied inside the aimbot's mouse path only. It never
    // reaches the trigger, the menu, or any other input the user sends.
    bool noise = HasOpt2(OPT2_AIM_NOISE);
    if (CometUi::Toggle("Input noise (aim only)",&noise,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_AIM_NOISE,noise);
    ImGui::BeginDisabled(!HasOpt2(OPT2_AIM_NOISE));
    SliderF32("Noise amount",g_cfg.noise_strength,0.0002f,0.008f,"%.4f");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Input method: SendInput");
    ImGui::TextWrapped("Mouse corrections are sent with SendInput only. The noise offset is bounded per correction and fades out at the aim point.");
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_recoil",ImVec2(0,0));
    Section("NO RECOIL");
    FlagCheck("Enable",F_RECOIL);
    Section("RCS");
    {
        bool rcs = g_cfg.humanize > 0.0f;
        if (CometUi::Toggle("Enable##rcs", &rcs, ImGui::GetContentRegionAvail().x)) {
            g_cfg.humanize = rcs ? 1.0f : 0.0f;
            g_cfg_dirty = true;
        }
    }
    EndCard();
    ImGui::EndTable();
}

static void DrawGunPane(const char* table_id, const std::vector<uint32_t>& ids, int& sel, int apply_type) {
    if (ids.empty()) { ImGui::TextDisabled("No skins"); return; }
    sel = std::clamp(sel, 0, (int)ids.size() - 1);
    if (!ImGui::BeginTable(table_id, 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("categories", ImGuiTableColumnFlags_WidthFixed, 175);
    ImGui::TableSetupColumn("skins", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextColumn();
    ImGui::BeginChild("categories", ImVec2(0, 0), ImGuiChildFlags_None);
    for (size_t i = 0; i < ids.size(); ++i) {
        auto found = g_gun_label.find(ids[i]);
        std::string label = found != g_gun_label.end() ? found->second : "Weapon";
        if (apply_type == 3) {
            switch (ids[i] / 100) { case 201: label = "Dagger & blades"; break; case 202: label = "Axe & claymore"; break;
            case 203: label = "Katana"; break; case 206: label = "Bat & club"; break; case 207: label = "Butterfly knife"; break; case 210: label = "Artifacts"; break; }
        } else {
            const auto split = label.find(" - "); if (split != std::string::npos) label.resize(split);
        }
        ImGui::PushID((int)i);
        if (ImGui::Selectable(label.c_str(), sel == (int)i, 0, ImVec2(0, 32))) sel = (int)i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("skin_list", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool artifact = apply_type == 3 && ids[(size_t)sel] / 100 == 210;
    auto found = g_gun_skins.find(ids[(size_t)sel]);
    std::vector<int> filtered_indices;
    if (found != g_gun_skins.end()) {
        filtered_indices.reserve(found->second.size());
        for (int index : found->second) {
            const auto& entry = g_guns[(size_t)index];
            const std::string name = NameOf(entry);
            if (ContainsNoCase(name, g_search)) {
                filtered_indices.push_back(index);
            }
        }
    }
    if (!filtered_indices.empty()) {
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_indices.size());
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                int index = filtered_indices[k];
                const auto& entry = g_guns[(size_t)index];
                const std::string name = NameOf(entry);
                ImGui::PushID(index);
                const float row_y = ImGui::GetCursorPosY(), width = ImGui::GetContentRegionAvail().x;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(80.f, width - 84));
                ImGui::BeginGroup();
                const bool equipped = std::any_of(g_skin_inventory.begin(), g_skin_inventory.end(), [&](const CatEntry& saved) {
                    return saved.a == static_cast<uint32_t>(apply_type) && saved.c == entry.a;
                });
                ImGui::BeginDisabled(artifact);
                if (ImGui::Selectable(name.c_str(), equipped, 0, ImVec2(std::max(80.f, width - 84), 0)))
                    ApplyItem(apply_type, entry.a);
                ImGui::EndDisabled();
                ImGui::EndGroup();
                ImGui::PopTextWrapPos();
                const float text_end = ImGui::GetCursorPosY();
                ImGui::SetCursorPos(ImVec2(std::max(0.f, width - 70), row_y + 4));
                ImGui::BeginDisabled(artifact);
                if (ImGui::Button("Apply", ImVec2(66, 30))) ApplyItem(apply_type, entry.a);
                ImGui::EndDisabled();
                ImGui::SetCursorPosY(std::max(row_y + 56, text_end + 8));
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        clipper.End();
    } else {
        ImGui::TextDisabled("No results");
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

static uint32_t g_skin_hero_cfg = 0;

static uint32_t g_skin_knife_cfg = 0;

static uint32_t g_skin_weapon_cfg = 0;

static int FindIdIndex(const std::vector<uint32_t>& ids, uint32_t item) {

    if (!item) return -1;

    for (size_t i = 0; i < ids.size(); ++i) {

        if (ids[i] == item) return (int)i;

    }

    return -1;

}

static int FindHeroSkinSel(uint32_t item) {

    if (!item) return -1;

    for (size_t i = 0; i < g_heroes.size(); ++i) {

        if (g_heroes[i].c == item) {

            int idx = FindIdIndex(g_hero_ids, g_heroes[i].a);

            if (idx >= 0) return idx;

        }

    }

    return -1;

}

static int FindGunSkinSel(const std::vector<uint32_t>& gunIds, uint32_t item) {

    if (!item) return -1;

    for (size_t gi = 0; gi < gunIds.size(); ++gi) {

        auto it = g_gun_skins.find(gunIds[gi]);

        if (it == g_gun_skins.end()) continue;

        for (int idx : it->second) {

            if (g_guns[(size_t)idx].a == item) return (int)gi;

        }

    }

    return -1;

}

static void SyncSkinSelections() {

    if (g_cfg.skin_hero_item && g_cfg.skin_hero_item != g_skin_hero_cfg) {

        int i = FindHeroSkinSel(g_cfg.skin_hero_item);

        if (i >= 0) g_hero_sel = i;

        g_skin_hero_cfg = g_cfg.skin_hero_item;

    }

    if (g_cfg.skin_knife_item && g_cfg.skin_knife_item != g_skin_knife_cfg) {

        int i = FindGunSkinSel(g_knife_ids, g_cfg.skin_knife_item);

        if (i >= 0) g_knife_sel = i;

        g_skin_knife_cfg = g_cfg.skin_knife_item;

    }

    if (g_cfg.skin_gun_item && g_cfg.skin_gun_item != g_skin_weapon_cfg) {

        int i = FindGunSkinSel(g_weapon_ids, g_cfg.skin_gun_item);

        if (i >= 0) g_weapon_sel = i;

        g_skin_weapon_cfg = g_cfg.skin_gun_item;

    }

}

static void DrawSkinInventory() {
    if (g_skin_inventory.empty()) {
        ImGui::TextDisabled("Empty");
        return;
    }
    int remove_index = -1;
    size_t visible = 0;
    const float height = std::max(120.f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("inventorylist", 2, ImGuiTableFlags_SizingStretchProp |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Skin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 74);
        for (size_t i = 0; i < g_skin_inventory.size(); ++i) {
            const CatEntry entry = g_skin_inventory[i];
            const std::string name = NameOf(entry);
            if (!ContainsNoCase(name, g_search)) continue;
            ++visible;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 60);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!g_inventory_writable);
            if (ImGui::Button("Remove", ImVec2(68, 28))) remove_index = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!visible) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No results");
        }
        ImGui::EndTable();
    }
    // Defer mutations until no table row references the saved vector.
    if (remove_index >= 0) {
        const CatEntry removed = g_skin_inventory[static_cast<size_t>(remove_index)];
        auto entries = g_skin_inventory;
        entries.erase(entries.begin() + remove_index);
        if (SaveSkinInventory(entries)) {
            uint32_t* last = removed.a == 1 ? &g_cfg.skin_hero_item : removed.a == 2 ? &g_cfg.skin_gun_item : &g_cfg.skin_knife_item;
            if (*last == removed.c) { *last = 0; g_cfg_dirty = true; SaveCfg(); }
        }
    }
}


static void DrawSkinTab() {
    LoadSkinInventory();
    MigrateSkinInventory();

    if (!g_catalog_ready) {

        ImGui::TextDisabled("Catalog not loaded.");

        if (ImGui::Button("Load catalog")) LoadCatalog();

    }

    if (g_catalog_ready) SyncSkinSelections();

    uint32_t cur = g_state.local_hero_id;

    bool cur_known = cur != 0 && g_hero_skins.find(cur) != g_hero_skins.end();



    ImGui::SetNextItemWidth(std::max(180.f, ImGui::GetContentRegionAvail().x - 132.f));
    ImGui::InputTextWithHint("##search", "Search skin name", g_search, sizeof(g_search));
    ImGui::SameLine();
    if (ImGui::Button("Reload catalog", ImVec2(122, 0))) LoadCatalog();
    bool only = Has(F_SKIN_CURRENT);
    if (CometUi::Toggle("Current hero only", &only)) SetF(F_SKIN_CURRENT, only);
    ImGui::SameLine();
    bool auto_apply = g_cfg.auto_skin != 0;
    if (CometUi::Toggle("Auto apply saved skins", &auto_apply)) { g_cfg.auto_skin = auto_apply ? 1u : 0u; g_cfg_dirty = true; SaveCfg(); }
    if (!g_inventory_error.empty()) {
        ImGui::TextWrapped("%s", g_inventory_error.c_str());
        if (ImGui::Button("Retry Inventory load")) LoadSkinInventory(true);
    }
    ImGui::Spacing();

    std::vector<uint32_t> ids;

    ids.reserve(g_hero_ids.size());

    for (uint32_t id : g_hero_ids) {

        if (only && cur_known && id != cur) continue;

        ids.push_back(id);

    }

    if (only && cur_known && ids.empty()) ids.push_back(cur);

    if (ids.empty()) ids = g_hero_ids;



    if (g_hero_sel < 0) g_hero_sel = 0;

    if (!ids.empty() && (size_t)g_hero_sel >= ids.size()) g_hero_sel = 0;

    if (only && cur_known) {

        for (size_t i = 0; i < ids.size(); ++i) {

            if (ids[i] == cur) { g_hero_sel = (int)i; break; }

        }

    }



    static int skin_page = 0;
    const char* skin_tabs[] = {"Characters", "Knives", "Guns", "Inventory"};
    const float tab_width = (ImGui::GetContentRegionAvail().x - 24.f) / 4.f;
    for (int i=0; i<4; ++i) {
        if(i) ImGui::SameLine(0,8);
        if(TabButton(skin_tabs[i],skin_page==i,tab_width)) skin_page=i;
    }
    ImGui::Dummy(ImVec2(0,12));
    {

        if (skin_page == 0) {

            if (only && !cur_known) {

                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f),

                                   "Hero unavailable");

            }

            if (ImGui::BeginTable("herotbl", 2, ImGuiTableFlags_SizingStretchProp)) {

                ImGui::TableSetupColumn("heroes", ImGuiTableColumnFlags_WidthFixed, 175.0f);

                ImGui::TableSetupColumn("skins", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();

                ImGui::TableNextColumn();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.075f, .075f, .085f, 1.f));

                ImGui::BeginChild("herolist", ImVec2(0, 0), ImGuiChildFlags_None);

                ImGui::PopStyleColor();

                std::vector<int> filtered_heroes;

                filtered_heroes.reserve(ids.size());

                for (size_t i = 0; i < ids.size(); ++i) {

                    uint32_t id = ids[i];

                    auto it = g_hero_skins.find(id);

                    const CatEntry* first = nullptr;

                    if (it != g_hero_skins.end() && !it->second.empty())

                        first = &g_heroes[(size_t)it->second[0]];

                    std::string label = HeroLabel(id, first);

                    if (ContainsNoCase(label, g_search)) {

                        filtered_heroes.push_back((int)i);

                    }

                }

                ImGuiListClipper h_clipper;

                h_clipper.Begin((int)filtered_heroes.size());

                while (h_clipper.Step()) {

                    for (int k = h_clipper.DisplayStart; k < h_clipper.DisplayEnd; ++k) {

                        int i = filtered_heroes[k];

                        uint32_t id = ids[(size_t)i];

                        auto it = g_hero_skins.find(id);

                        const CatEntry* first = nullptr;

                        if (it != g_hero_skins.end() && !it->second.empty())

                            first = &g_heroes[(size_t)it->second[0]];

                        std::string label = HeroLabel(id, first);

                        char full[160];

                        snprintf(full, sizeof(full), "%s##h%u", label.c_str(), id);

                        ImGui::PushID(i);

                        if (ImGui::Selectable(full, i == g_hero_sel, 0, ImVec2(0, 22))) g_hero_sel = i;

                        ImGui::PopID();

                    }

                }

                h_clipper.End();

                ImGui::EndChild();



                ImGui::TableNextColumn();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.075f, .075f, .085f, 1.f));

                ImGui::BeginChild("heroskins", ImVec2(0, 0), ImGuiChildFlags_None);

                ImGui::PopStyleColor();

                uint32_t sel_id = 0;

                if (g_hero_sel >= 0 && (size_t)g_hero_sel < ids.size()) sel_id = ids[(size_t)g_hero_sel];

                std::vector<int> filtered_hskins;

                if (sel_id) {

                    auto itmap = g_hero_skins.find(sel_id);

                    if (itmap != g_hero_skins.end()) {

                        for (int idx : itmap->second) {

                            const auto& e = g_heroes[(size_t)idx];

                            std::string name = NameOf(e);

                            if (name.empty()) name = "Default skin";

                            if (ContainsNoCase(name, g_search)) {

                                filtered_hskins.push_back(idx);

                            }

                        }

                    }

                }

                if (!filtered_hskins.empty()) {

                    ImGuiListClipper hs_clipper;

                    hs_clipper.Begin((int)filtered_hskins.size());

                    while (hs_clipper.Step()) {

                        for (int k = hs_clipper.DisplayStart; k < hs_clipper.DisplayEnd; ++k) {

                            int idx = filtered_hskins[k];

                            const auto& e = g_heroes[(size_t)idx];

                            std::string name = NameOf(e);

                            if (name.empty()) name = "Default skin";

                            ImGui::PushID(idx);

                            const bool equipped = std::any_of(g_skin_inventory.begin(), g_skin_inventory.end(), [&](const CatEntry& saved) {
                                return saved.a == 1 && saved.c == e.c;
                            });
                            const float skin_width = std::max(80.f, ImGui::GetContentRegionAvail().x - 70.f);
                            if (ImGui::Selectable(name.c_str(), equipped, 0, ImVec2(skin_width, 0))) ApplyItem(1, e.c);

                            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 62.0f);

                            if (ImGui::Button("Apply", ImVec2(58, 0))) ApplyItem(1, e.c);

                            ImGui::PopID();

                        }

                    }

                    hs_clipper.End();

                } else if (sel_id) {

                    ImGui::TextDisabled("No results");

                }

                uint32_t restore_model = 0;

                auto itdef = g_hero_skins.find(sel_id);

                if (itdef != g_hero_skins.end()) {

                    for (int idx : itdef->second) {

                        const auto& e = g_heroes[(size_t)idx];

                        if (e.b == 0) { restore_model = e.c; break; }

                    }

                    if (!restore_model && !itdef->second.empty())

                        restore_model = g_heroes[(size_t)itdef->second[0]].c;

                }

                if (restore_model) {

                    if (ImGui::Button("Restore default model", ImVec2(180, 0))) ApplyItem(1, restore_model);

                }

                ImGui::EndChild();

                ImGui::EndTable();

            }



        }

        if (skin_page == 1) {

            DrawGunPane("knifetbl", g_knife_ids, g_knife_sel, 3);



        }

        if (skin_page == 2) {

            DrawGunPane("weapontbl", g_weapon_ids, g_weapon_sel, 2);



        }

        if (skin_page == 3) {
            DrawSkinInventory();

        }



    }

}



static DWORD WINAPI UnloadWorker(LPVOID) {

    Sleep(100);

    comet_capture::Restore(g_hwnd, g_obs);
    comet_surface::Destroy(g_surface);

    if (g_hwnd && g_wndproc) {

        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_wndproc);

        g_wndproc = nullptr;

    }

    MH_DisableHook(MH_ALL_HOOKS);

    MH_Uninitialize();

    if (g_imgui_ready) {

        ImGui_ImplDX11_Shutdown();

        ImGui_ImplWin32_Shutdown();

        ImGui::DestroyContext();

        g_imgui_ready = false;

    }

    g_rtv_live = false;

    if (g_context) { g_context->Release(); g_context = nullptr; }

    if (g_device) { g_device->Release(); g_device = nullptr; }

    if (g_shm_state_p) { UnmapViewOfFile(g_shm_state_p); g_shm_state_p = nullptr; }

    if (g_shm_state_h) { CloseHandle(g_shm_state_h); g_shm_state_h = nullptr; }

    if (g_shm_cfg_p) { UnmapViewOfFile(g_shm_cfg_p); g_shm_cfg_p = nullptr; }

    if (g_shm_cfg_h) { CloseHandle(g_shm_cfg_h); g_shm_cfg_h = nullptr; }



    SetF(F_EXIT_CHEAT, true);

    SaveCfg();



    Log("overlay unloaded cleanly");

    Sleep(200);

    FreeLibraryAndExitThread(g_hmodule, 0);

    return 0;

}



static void SafeExitCheatAndGame() {
    g_unloading=true;
    comet_capture::Restore(g_hwnd, g_obs);
    comet_surface::Destroy(g_surface);
    if (g_hwnd && g_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_wndproc);
        g_wndproc = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imgui_ready) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_ready = false;
    }
    g_rtv_live = false;
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }

    if (g_shm_state_p) { UnmapViewOfFile(g_shm_state_p); g_shm_state_p = nullptr; }
    if (g_shm_state_h) { CloseHandle(g_shm_state_h); g_shm_state_h = nullptr; }
    if (g_shm_cfg_p) { UnmapViewOfFile(g_shm_cfg_p); g_shm_cfg_p = nullptr; }
    if (g_shm_cfg_h) { CloseHandle(g_shm_cfg_h); g_shm_cfg_h = nullptr; }

    std::vector<std::wstring> files = {
        StatePath(), CfgPath(), CatalogPath(),
        IpcDir() + L"\\runtime_err.txt",
        LogPath(),
        PathBase() + L"\\pyexec\\out.txt",
        PathBase() + L"\\pyexec\\booted.txt"
    };
    for (const auto& fpath : files) {
        DeleteFileW(fpath.c_str());
    }

    TerminateProcess(GetCurrentProcess(), 0);
}

static void UnloadOverlay() {

    if (g_unloading) return;

    g_unloading = true;

    HANDLE t = CreateThread(nullptr, 0, UnloadWorker, nullptr, 0, nullptr);

    if (t) CloseHandle(t);

}



static void DrawSettingsTab() {
    if(!ImGui::BeginTable("settings_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("settings_general",ImVec2(0,0));
    Section("Configuration");
    BindRow("Menu key",g_cfg.menu_key,3);
    if(g_bind_slot==3) ImGui::TextDisabled("Press a key to assign...");
    ImGui::Dummy(ImVec2(0,12));
    if(ImGui::Button("Save configuration",ImVec2(-1,34))) SaveConfiguration();
    if(!g_cfg_error.empty()) ImGui::TextWrapped("%s",g_cfg_error.c_str());
    ImGui::Dummy(ImVec2(0,12));
    if(ImGui::Button("Reset to defaults",ImVec2(-1,32))) ResetCfg();
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("settings_appearance",ImVec2(0,0));
    Section("Appearance");
    const char* themes[]={"Purple","Crimson","Cyan","Emerald","Mono"};
    int theme=g_cfg.pad[0]==CFG_UI_TAG&&g_cfg.pad[1]<=4?static_cast<int>(g_cfg.pad[1]):2;
    ImGui::TextUnformatted("Accent color"); ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##accent",&theme,themes,5)) {
        g_cfg.pad[0]=CFG_UI_TAG;g_cfg.pad[1]=theme;g_cfg_dirty=true;ApplyMenuPreferences();
    }
    bool reduced=g_cfg.pad[0]==CFG_UI_TAG&&g_cfg.pad[2]!=0;
    if(CometUi::Toggle("Reduced motion",&reduced,ImGui::GetContentRegionAvail().x)) {
        g_cfg.pad[0]=CFG_UI_TAG;g_cfg.pad[1]=theme;g_cfg.pad[2]=reduced?1u:0u;
        g_cfg_dirty=true;ApplyMenuPreferences();
    }
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("settings_perf",ImVec2(0,0));
    Section("Performance");
    bool no_vsync = HasOpt2(OPT2_NO_VSYNC);
    if (CometUi::Toggle("Ignore game V-Sync",&no_vsync,ImGui::GetContentRegionAvail().x)) {
        SetOpt2(OPT2_NO_VSYNC,no_vsync);
        Log("vsync override %s", no_vsync ? "on" : "off");
    }
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("settings_protection",ImVec2(0,0));
    Section("Protection");

    // One switch, one behaviour: the overlay is drawn into its own window, so the
    // game's swap-chain back buffer - what OBS Game Capture, Steam's F12 and any
    // other in-process Present hook copy - never contains it. That window also
    // hides itself while a screenshot is being taken, so a snip comes back as the
    // game with no ESP and no black rectangle.
    bool protect = HasOpt2(OPT2_CAPTURE_SHIELD);
    if (CometUi::Toggle("Hide ESP from screen capture",&protect,ImGui::GetContentRegionAvail().x)) {
        SetOpt2(OPT2_CAPTURE_SHIELD,protect);
        // The older, narrower switches are folded into this one.
        SetOpt2(OPT2_GDI_GUARD,false);
        SetOpt2(OPT2_OBS_PROTECT,false);
        // Record the wish only. This runs *inside* comet_surface::Begin..End for
        // the frame being drawn, with the shield's render target bound: creating
        // or destroying the surface here pulled the bound view and the swap chain
        // out from under the draw data that was already recorded and, on the way
        // out, skipped the restore of the game's render target - which is what
        // left the menu frozen and the game drawing into a dead buffer.
        // MaintainShieldSurface() applies it at the top of the next frame, before
        // anything is bound.
        g_surface_retry_at = 0;
        Log("capture shield %s", protect ? "requested: on" : "requested: off");
    }
    if (protect && !g_clean_tex) {
        // One short line, only while the first drawn frame is still pending.
        ImGui::TextDisabled("armed");
    }

    FlagCheck("Block reports",F_BLOCK_REPORT);
    FlagCheck("Unlock effects",F_UNLOCK_EFFECTS);
    FlagCheck("Force killfx",F_FORCE_KILLFX);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("settings_session",ImVec2(0,0));
    Section("Session controls");
    if(ImGui::Button("Unload overlay",ImVec2(-1,34))) UnloadOverlay();
    ImGui::Dummy(ImVec2(0,8));
    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(CometUi::Danger));
    if(ImGui::Button("Safe exit",ImVec2(-1,34))) SafeExitCheatAndGame();
    ImGui::PopStyleColor();
    EndCard();
    ImGui::EndTable();
}

static void DrawMenu() {
    if (!Has(F_MENU_OPEN)) return;
    // Catalog hot-reload. This is a synchronous file read plus a rebuild of the
    // skin index, so it runs on a 2 s timer and only when the file really
    // changed - not once per frame while the menu is open.
    static ULONGLONG next_catalog_check = 0;
    static FILETIME last_catalog_write{};
    const ULONGLONG now_tick = GetTickCount64();
    if (now_tick >= next_catalog_check) {
        next_catalog_check = now_tick + 2000;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(CatalogPath().c_str(), GetFileExInfoStandard, &attr) &&
            (!g_catalog_ready || CompareFileTime(&last_catalog_write, &attr.ftLastWriteTime) != 0)) {
            LoadCatalog();
            last_catalog_write = attr.ftLastWriteTime;
        }
    }
    ProcessBind();
    auto& io = ImGui::GetIO();
    // Focus gate: dim the game behind the menu.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    const float max_w = std::max(640.f, io.DisplaySize.x - 24.f);
    const float max_h = std::max(420.f, io.DisplaySize.y - 24.f);
    ImGui::SetNextWindowSize(ImVec2(std::min(960.f, max_w), std::min(680.f, max_h)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(820.f, max_w), std::min(560.f, max_h)), ImVec2(max_w, max_h));
    if (g_reset_menu_pos) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
        g_reset_menu_pos = false;
    } else ImGui::SetNextWindowPos(ImVec2(60, 30), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##comet_main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();

    constexpr float rail = 188;
    dl->AddRectFilled(wp, ImVec2(wp.x + rail, wp.y + ws.y), CometUi::Ground, 12, ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(wp.x + rail, wp.y), ImVec2(wp.x + rail, wp.y + ws.y), CometUi::Border);
    CometUi::Logo(dl,ImVec2(wp.x+22,wp.y+26),30);
    if(g_font_big) dl->AddText(g_font_big,20,ImVec2(wp.x+64,wp.y+24),CometUi::Text,"Comet");
    else dl->AddText(ImVec2(wp.x+64,wp.y+24),CometUi::Text,"Comet");
    ImGui::SetCursorPos(ImVec2(12,100));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6));
    ImGui::BeginChild("navigation", ImVec2(rail - 24, ws.y - 120), ImGuiChildFlags_None);
    const char* labels[] = {"Visuals", "Aim", "Skins", "Settings"};
    const CometUi::Icon icons[] = {CometUi::Icon::Eye, CometUi::Icon::Target, CometUi::Icon::Skins, CometUi::Icon::Settings};
    CometUi::NavIndicator(g_tab, rail - 24);
    for (int i = 0; i < 4; ++i) {
        if (CometUi::NavItem(labels[i], icons[i], g_tab == i, rail - 24)) g_tab = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorPos(ImVec2(rail+26,22));
    if(g_font_big) ImGui::PushFont(g_font_big,22);
    ImGui::TextUnformatted(labels[std::clamp(g_tab,0,3)]);
    if(g_font_big) ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ws.x-178,26));
    if(ImGui::Button("Save config",ImVec2(112,32))) SaveConfiguration();
    ImGui::SetCursorPos(ImVec2(ws.x-46,28));
    if(CometUi::CloseButton("##close",ImVec2(28,28))) SetF(F_MENU_OPEN,false);
    dl->AddLine(ImVec2(wp.x+rail+26,wp.y+70),ImVec2(wp.x+ws.x-26,wp.y+70),CometUi::Border);
    ImGui::SetCursorPos(ImVec2(rail+26,86));
    ImGui::PushID(g_tab);
    ImGui::BeginChild("content", ImVec2(ws.x - rail - 52, ws.y - 104), ImGuiChildFlags_None);
    switch (g_tab) { case 0: DrawESPTab(); break; case 1: DrawAimTab(); break;
                    case 2: DrawSkinTab(); break; default: DrawSettingsTab(); break; }
    ImGui::EndChild(); ImGui::PopID(); ImGui::End(); ImGui::PopStyleVar();
}



static void HandleToggle() {

    if (!g_cfg_ready) return;

    static bool prev = false;

    int key = g_cfg.menu_key;

    if (key < 0 || key > 255) key = VK_INSERT;

    bool down = (RawAsyncKeyState(key) & 0x8000) != 0;

    if (down && !prev) {

        SetF(F_MENU_OPEN, !Has(F_MENU_OPEN));

    }

    prev = down;

}



// Confirms buffer 0 can be turned into a render target and records the current
// size, then releases the view immediately. Nothing here is kept: see the note
// on g_rtv_live. If this fails, drawing is skipped for the frame rather than
// leaving a held reference behind.
static void CreateRTV() {

    g_rtv_live = false;

    if (!g_swapchain || !g_device) return;

    ID3D11Texture2D* back = nullptr;

    if (FAILED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;

    D3D11_TEXTURE2D_DESC td{};

    back->GetDesc(&td);

    if (td.Width > 0 && td.Height > 0) {

        g_width = td.Width;

        g_height = td.Height;

    }

    ID3D11RenderTargetView* probe = nullptr;

    if (SUCCEEDED(g_device->CreateRenderTargetView(back, nullptr, &probe)) && probe) {

        g_rtv_live = true;

        probe->Release();

    }

    back->Release();

}




static bool BindSwapchain(IDXGISwapChain* sc) {

    if (!sc) return false;

    if (sc == g_swapchain) return true;

    // Rejection cache. Present runs at the refresh rate; a swapchain we already
    // refused (another window, another device) must not be re-interrogated, and
    // above all must not log: Log() opens, writes and closes the log file, which
    // is a synchronous disk write inside Present. Re-logging every present of a
    // non-selected swapchain was a direct source of frame drops.
    static IDXGISwapChain* rejected[8]{};
    static int rejected_count = 0;
    for (int i = 0; i < rejected_count; ++i) {

        if (rejected[i] == sc) return false;

    }

    auto reject = [&]() {

        if (rejected_count < (int)(sizeof(rejected) / sizeof(rejected[0]))) rejected[rejected_count++] = sc;

        return false;

    };

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(sc->GetDesc(&desc))) return reject();

    if (g_hwnd && desc.OutputWindow && desc.OutputWindow != g_hwnd) {

        Log("swapchain skip other hwnd=%p", desc.OutputWindow);

        return reject();

    }

    ID3D11Device* dev = nullptr;

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {

        Log("swapchain skip no device %p", sc);

        return reject();

    }

    bool same_device = (dev == g_device);

    dev->Release();

    if (!same_device) {

        Log("swapchain skip other device %p", sc);

        return reject();

    }

    g_rtv_live = false;

    g_swapchain = sc;

    Log("swapchain rebind %p", sc);
    LogDisplayMode(true);

    return true;

}



static void UpdateClientSize() {

    if (!g_hwnd) return;

    RECT rc{};

    if (GetClientRect(g_hwnd, &rc)) {

        UINT w = (UINT)std::max<LONG>(0, rc.right - rc.left);

        UINT h = (UINT)std::max<LONG>(0, rc.bottom - rc.top);

        if (w > 0 && h > 0) {

            g_client_w = w;

            g_client_h = h;

            return;

        }

    }

    if (g_width > 0 && g_height > 0) {

        g_client_w = g_width;

        g_client_h = g_height;

    }

}



// ---------------------------------------------------------------------------
// Fonts.
//
// Player names in this game are not ASCII: Chinese, Japanese and Korean names
// are the norm, names carry emoji and decorative symbols, and the ESP used to
// render all of it with a Latin-only atlas - missing glyphs came out as nothing
// or as boxes. Coverage is now explicit and split by script so each fallback
// font only rasterises what it owns; handing every merged font the full Unicode
// range is what turns an 18 px name atlas into a 64 MB texture.
//
// The UI font stays Latin + symbols (the menu has no user text); only the name
// font pays for CJK, Hangul and emoji.
// ---------------------------------------------------------------------------
namespace fontcov {

inline ImFontAtlas* Atlas() { return ImGui::GetIO().Fonts; }

// Latin, punctuation, currency, arrows, math, geometric shapes, dingbats and the
// CJK punctuation block: everything a "special symbols" name is built from.
inline ImVector<ImWchar>& Latin() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(Atlas()->GetGlyphRangesDefault());
        static const ImWchar extra[] = {
            0x0100, 0x024F,   // Latin extended A/B
            0x0370, 0x03FF,   // Greek
            0x0400, 0x04FF,   // Cyrillic
            0x1E00, 0x1EFF,   // Latin extended additional (Vietnamese)
            0x2000, 0x206F,   // general punctuation
            0x20A0, 0x20CF,   // currency
            0x2100, 0x214F,   // letterlike
            0x2190, 0x21FF,   // arrows
            0x2200, 0x22FF,   // math operators
            0x2300, 0x23FF,   // misc technical
            0x2460, 0x24FF,   // enclosed alphanumerics
            0x2500, 0x257F,   // box drawing
            0x25A0, 0x25FF,   // geometric shapes
            0x2600, 0x26FF,   // misc symbols
            0x2700, 0x27BF,   // dingbats
            0x2900, 0x297F,   // supplemental arrows
            0x2B00, 0x2BFF,   // misc symbols and arrows
            0x3000, 0x303F,   // CJK punctuation
            0
        };
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }
    return ranges;
}

// Chinese (common set), Japanese (kana + common kanji) and Korean (common
// syllables). About eight thousand glyphs: enough for real names, small enough
// to bake in well under a second.
inline ImVector<ImWchar>& Names() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(Latin().Data);
        b.AddRanges(Atlas()->GetGlyphRangesChineseSimplifiedCommon());
        b.AddRanges(Atlas()->GetGlyphRangesJapanese());
        b.AddRanges(Atlas()->GetGlyphRangesKorean());
        static const ImWchar cjk_extra[] = {
            0x2E80, 0x2EFF,   // CJK radicals supplement
            0x3100, 0x312F,   // bopomofo
            0x31A0, 0x31BF,   // bopomofo extended
            0x31F0, 0x31FF,   // katakana phonetic extensions
            0x3200, 0x32FF,   // enclosed CJK
            0xF900, 0xFAFF,   // CJK compatibility ideographs
            0xFF00, 0xFFEF,   // halfwidth and fullwidth forms
            0
        };
        b.AddRanges(cjk_extra);
#ifdef IMGUI_USE_WCHAR32
        // Emoji planes. Only addressable with 32-bit ImWchar. Most Windows emoji
        // are colour glyphs that stb_truetype cannot rasterise, but the
        // monochrome ones (and everything in Segoe UI Symbol) do render, so a
        // plain fallback beats dropping the character.
        static const ImWchar emoji[] = {
            0x1F000, 0x1F0FF, 0x1F100, 0x1F1FF, 0x1F300, 0x1F5FF,
            0x1F600, 0x1F64F, 0x1F680, 0x1F6FF, 0x1F900, 0x1F9FF,
            0x1FA70, 0x1FAFF, 0
        };
        b.AddRanges(emoji);
#endif
        b.BuildRanges(&ranges);
    }
    return ranges;
}

inline const std::string& Root() {
    static std::string root;
    if (root.empty()) {
        char dir[MAX_PATH]{};
        const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
        if (n && n < MAX_PATH) root = std::string(dir) + "\\Fonts\\";
    }
    return root;
}

inline bool Exists(const char* file) {
    const std::string& root = Root();
    if (root.empty()) return false;
    return GetFileAttributesA((root + file).c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline ImFont* Add(const char* file, float size, const ImWchar* ranges, bool merge,
                   float oversample = 2.0f) {
    if (!Exists(file)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = oversample;
    cfg.OversampleV = oversample;
    cfg.PixelSnapH = true;
    cfg.MergeMode = merge;
    return Atlas()->AddFontFromFileTTF((Root() + file).c_str(), size, &cfg, ranges);
}

// Primary face, first one that loads. Bold first: names are read at a glance.
inline ImFont* Primary(float size, const ImWchar* ranges) {
    static const char* candidates[] = {"seguisb.ttf", "segoeuib.ttf", "segoeui.ttf",
                                       "msyhbd.ttc", "msyh.ttc", "simhei.ttf"};
    for (const char* file : candidates) {
        if (ImFont* f = Add(file, size, ranges, false)) return f;
    }
    return nullptr;
}

// Script fallbacks for names. Each is handed the whole name range and the atlas
// keeps the first font that has a glyph, so the order below is the priority:
// YaHei covers most Chinese, kana and common kanji; the rest fill the gaps.
inline void MergeNameFallbacks(float size) {
    const ImVector<ImWchar>& names = Names();
    static const char* fonts[] = {
        "msyh.ttc",     // Microsoft YaHei: Chinese, kana, common kanji
        "msjh.ttc",     // Microsoft JhengHei: Traditional Chinese
        "simsun.ttc",   // SimSun: last-resort CJK
        "malgun.ttf",   // Malgun Gothic: Hangul
        "gulim.ttc",    // Gulim: older Hangul
        "seguisym.ttf", // arrows, geometric shapes, dingbats
        "seguiemj.ttf", // emoji (monochrome subset)
    };
    for (const char* file : fonts) Add(file, size, names.Data, true, 1.0f);
}

} // namespace fontcov

static void InitFonts() {

    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontDefault();
    if (fontcov::Root().empty()) return;

    // Menu: Latin + symbols only. No user text is drawn with these.
    ImFont* ui = fontcov::Primary(16.0f, fontcov::Latin().Data);
    if (ui) io.FontDefault = ui;

    ImFont* big = fontcov::Primary(21.0f, fontcov::Latin().Data);
    if (big) g_font_big = big;

    // Names: full coverage, merged into one font so AddText never switches face.
    ImFont* esp = fontcov::Primary(18.0f, fontcov::Names().Data);
    if (esp) {
        g_font_esp = esp;
        fontcov::MergeNameFallbacks(18.0f);
    }

}

static SHORT RawAsyncKeyState(int vk) {return GetAsyncKeyState(vk);}

static LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {

    if(m==MenuInputCloseMessage()) {
        if(g_menu_owns_capture&&GetCapture()==h) ReleaseCapture();
        g_menu_owns_capture=false;
        return 0;
    }
    if (g_unloading) return CallWindowProcW(g_wndproc, h, m, w, l);

    if (m == WM_KEYDOWN && w == VK_ESCAPE && Has(F_MENU_OPEN)) {

        SetF(F_MENU_OPEN, false);

        return 0;

    }

    if (m == WM_SIZE || m == WM_DISPLAYCHANGE || m == WM_DPICHANGED) {

        g_display_dirty = true;

        g_reset_menu_pos = true;

    }


    // A close-button press can close the menu before its release arrives.
    if(!Has(F_MENU_OPEN)&&g_imgui_ready&&(m==WM_LBUTTONUP||m==WM_RBUTTONUP||m==WM_MBUTTONUP||
       m==WM_XBUTTONUP||m==WM_CAPTURECHANGED||m==WM_KILLFOCUS||m==WM_KEYUP||m==WM_SYSKEYUP))
        ImGui_ImplWin32_WndProcHandler(h,m,w,l);

    if (Has(F_MENU_OPEN)) {

        if (g_bind_slot && g_bind_armed) {

            if (m == WM_LBUTTONDOWN) { CaptureBind(VK_LBUTTON); return 0; }

            if (m == WM_RBUTTONDOWN) { CaptureBind(VK_RBUTTON); return 0; }

            if (m == WM_MBUTTONDOWN) { CaptureBind(VK_MBUTTON); return 0; }

            if (m == WM_XBUTTONDOWN) { CaptureBind(HIWORD(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2); return 0; }

        }

        const HWND previous_capture=GetCapture();
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        if(!previous_capture&&GetCapture()==h) g_menu_owns_capture=true;
        const auto& io=ImGui::GetIO();
        const bool editing=io.WantTextInput||(g_bind_slot&&g_bind_armed);

        // Raw mouse and keyboard share WM_INPUT; menu visibility alone must not consume keyboard.
        if(m==WM_INPUT) {
            RAWINPUTHEADER header{};UINT bytes=sizeof(header);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_HEADER,&header,&bytes,sizeof(header))==sizeof(header)) {
                if(header.dwType==RIM_TYPEMOUSE)
                    return DefWindowProcW(h,m,w,l);
                if(header.dwType==RIM_TYPEKEYBOARD&&editing) {
                    RAWINPUT raw{};bytes=sizeof(raw);
                    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&raw,&bytes,sizeof(header))!=UINT(-1)&&
                       !(raw.data.keyboard.Flags&RI_KEY_BREAK)) return DefWindowProcW(h,m,w,l);
                }
            }
            return CallWindowProcW(g_wndproc,h,m,w,l);
        }
        if(m==WM_KEYUP||m==WM_SYSKEYUP) return CallWindowProcW(g_wndproc,h,m,w,l);
        if(m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_CHAR||m==WM_SYSCHAR) {
            if(editing) return 0;
            return CallWindowProcW(g_wndproc,h,m,w,l);
        }

        switch (m) {

            case WM_MOUSEMOVE:

            case WM_LBUTTONDOWN:

            case WM_LBUTTONUP:

            case WM_RBUTTONDOWN:

            case WM_RBUTTONUP:

            case WM_MBUTTONDOWN:

            case WM_MBUTTONUP:

            case WM_MOUSEWHEEL:

            case WM_XBUTTONDOWN:

            case WM_XBUTTONUP:

                return 0;

            case WM_SETCURSOR:

                SetCursor(nullptr);

                return TRUE;

            default:

                break;

        }

    }

    return CallWindowProcW(g_wndproc, h, m, w, l);

}



static void EnsureWndProc() {
    if(!g_hwnd) return;
    WNDPROC current=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC));
    if(!Has(F_MENU_OPEN)||g_unloading) {
        if(g_input_attached&&current==HookWndProc) {
            DWORD_PTR result=0;
            SendMessageTimeoutW(g_hwnd,MenuInputCloseMessage(),0,0,SMTO_ABORTIFHUNG,1000,&result);
            if(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC))==HookWndProc) {
                SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g_wndproc));
                g_input_attached=false;
                Log("menu closed: native game input restored flags=%08x",g_cfg.flags);
            }
        }
        return;
    }
    // If another overlay chained this handler, do not install above it again.
    if(g_input_attached) return;
    if(current!=HookWndProc) {
        g_wndproc=current;
        SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(HookWndProc));
    }
    g_input_attached=true;
    Log("menu opened: input handler attached");
}

static bool InitOverlay(IDXGISwapChain* sc) {

    if (g_imgui_ready) return true;

    if (g_init_failed) return false;

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {

        g_init_failed = true;

        Log("init failed: no output window");

        return false;

    }

    // This is the game's actual swap chain, not the hidden probe created by
    // InstallHook. Record its settings rather than assuming a presentation mode.
    Log("game swapchain effect=%u buffers=%u windowed=%d flags=0x%08x size=%ux%u hwnd=%p",
        (unsigned)desc.SwapEffect, (unsigned)desc.BufferCount, (int)desc.Windowed,
        (unsigned)desc.Flags, (unsigned)desc.BufferDesc.Width,
        (unsigned)desc.BufferDesc.Height, desc.OutputWindow);

    g_swapchain = sc;

    g_hwnd = desc.OutputWindow;
    // Display affinity belongs to capture_guard now: it applies the saved
    // OBS-protection setting instead of unconditionally clearing it, and
    // re-asserts it if the game drops it (see ApplyObsProtection).

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device) {

        g_init_failed = true;

        Log("init failed: GetDevice");

        return false;

    }

    g_device->GetImmediateContext(&g_context);

    if (!g_context) {

        g_init_failed = true;

        Log("init failed: GetImmediateContext");

        return false;

    }

    CreateRTV();

    if (!g_rtv_live) {

        g_init_failed = true;

        Log("init failed: rtv");

        return false;

    }

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    io.IniFilename = nullptr;

    io.LogFilename = nullptr;

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    CometUi::ApplyStyle();
    // Every control press lands in overlay_log.txt as "[ui] <label>".
    CometUi::SetTrace(TraceUi);

    InitFonts();

    ImGui_ImplWin32_Init(g_hwnd);

    if (!ImGui_ImplDX11_Init(g_device, g_context)) {

        g_init_failed = true;

        Log("init failed: ImGui_ImplDX11_Init");

        return false;

    }

    EnsureWndProc();

    InitSharedMemory();

    LoadCfg();
    EnsureWndProc();
    ApplyMenuPreferences();
    SaveCfg();

    LoadCatalog();

    LoadState();

    g_imgui_ready = true;

    Log("overlay ready hwnd=%p device=%p rtv=%d size=%ux%u", g_hwnd, g_device, g_rtv_live ? 1 : 0, g_width, g_height);

    return true;

}



// Per-frame cost split (CPU, milliseconds). Written by RenderFrame on the
// presenting thread and read by NotePresentPerf on the same thread.
static thread_local LARGE_INTEGER g_frame_start_qpc{};
static thread_local double t_ui_ms = 0.0;

// Split out of the pre-draw block: the state poll and the clean copy are the two
// pieces that can each cost milliseconds, and they are invisible in `pre_avg_ms`.
static thread_local double t_state_ms = 0.0;
static thread_local double t_copy_ms = 0.0;
static thread_local double t_pre_ms = 0.0;
static thread_local double t_shield_begin_ms = 0.0;
static thread_local double t_shield_present_ms = 0.0;

// Once per half second while the menu is open: proves the render loop reached
// this frame. If the log stops at "frame start" after a click, the crash is inside
// the overlay's own frame; if it keeps counting, the overlay is innocent and the
// fault is in the game or the runtime.
static void TraceFrameStart() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500) return;
    last = now;
    Log("[ui] frame start menu=%d surface=%d", Has(F_MENU_OPEN) ? 1 : 0,
        ShieldSurfaceReady() ? 1 : 0);
}

static void RenderFrame(IDXGISwapChain* sc) {

    t_ui_ms = t_shield_begin_ms = t_shield_present_ms = 0.0;
    t_state_ms = t_copy_ms = 0.0;
    QueryPerformanceCounter(&g_frame_start_qpc);

    if(!BindSwapchain(sc)) return;

    HandleToggle();
    if (Has(F_MENU_OPEN)) TraceFrameStart();

    if (!g_imgui_ready) return;

    ULONGLONG tick = GetTickCount64();

    bool menu = Has(F_MENU_OPEN);

    bool visual = menu || Has(F_ESP) || Has(F_AIM_FOV_CIRCLE);

    ULONGLONG state_interval = visual ? 0 : 250;

    if (tick - g_last_state_poll >= state_interval) {

        g_last_state_poll = tick;

        LARGE_INTEGER s0{}, s1{}, sf{};
        QueryPerformanceFrequency(&sf);
        QueryPerformanceCounter(&s0);
        LoadState();
        QueryPerformanceCounter(&s1);
        if (sf.QuadPart) t_state_ms = (double)(s1.QuadPart - s0.QuadPart) * 1000.0 / (double)sf.QuadPart;

    }

    // One save path, one cadence. A slider drag used to rewrite both config files
    // (IPC + LOCALAPPDATA) on the render thread while the menu was open, which is
    // a synchronous disk write inside Present - visible as a periodic hitch.
    if (g_cfg_dirty && tick - g_last_cfg_save >= 250) {

        g_last_cfg_save = tick;

        SaveCfg();

    }

    {
        // Subclassing check: the game's window procedure cannot change under us
        // more than once, so once a second is plenty. It used to run on every
        // presented frame.
        static ULONGLONG last_wnd_check = 0;
        if (tick - last_wnd_check >= 1000) { last_wnd_check = tick; EnsureWndProc(); }
    }
    MaintainShieldSurface();
    ApplyObsProtection();

    // Leaving the match is the reliable boundary for a new "Team 1": the label
    // cache is per-match, so it is cleared here as well as after a long empty
    // stretch (Frame() below), which covers wiping back to spectate.
    if (!g_state.in_match) g_team_labels.Reset();

    // Measures our own projection against the engine's, and nothing else. See
    // LogProjectionCalibration(). Throttled to once per 10 s.
    LogProjectionCalibration();

    bool need = menu || (g_state.in_match &&
                         (Has(F_ESP) || Has(F_AIM_FOV_CIRCLE)));

    // Everything above this point runs on every present, drawing or not (config
    // autosave, state poll, shield upkeep). Timing it separately is what tells a
    // fixed per-frame cost apart from the cost of actually drawing.
    {
        LARGE_INTEGER now_qpc{}, freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now_qpc);
        if (g_frame_start_qpc.QuadPart && freq.QuadPart)
            t_pre_ms = (double)(now_qpc.QuadPart - g_frame_start_qpc.QuadPart) * 1000.0 /
                       (double)freq.QuadPart;
    }

    // Diagnostics for the case where the menu draws but nothing else does: ESP on,
    // in a match, and the runtime has never sent an entity. Says so once every 10 s
    // instead of leaving the user with a menu and no features.
    if (Has(F_ESP) && g_state.in_match && g_ents.empty()) {
        static ULONGLONG last_empty_log = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last_empty_log > 10000) {
            last_empty_log = now;
            Log("state: ESP on, in match, no entities from the runtime (seq=%u count=%u)",
                (unsigned)g_state.seq, (unsigned)g_state.count);
        }
    }

    if (!need) {
        // Nothing to draw this frame. Take the protected window off the screen
        // so it cannot leave the last ESP frame standing there as a still.
        if (ShieldSurfaceReady()) comet_surface::Hide(g_surface);
        return;
    }

    // A desktop snip (Win+Shift+S, PrtScn, the Snipping Tool's own window) reads
    // the window content, not the swap chain, so a substituted back buffer does
    // not help it: the only way for that screenshot to be clean is for the overlay
    // not to be drawn while it is being taken. Cheap: the hotkey check is four
    // GetAsyncKeyState calls and the process/window scans are cached.
    {
        static ULONGLONG snip_since = 0;
        static bool snip_logged = false;
        const bool snip = CaptureProtectionOn() && comet_surface::SystemCaptureUiActive();
        const ULONGLONG now = GetTickCount64();
        if (!snip) {
            snip_since = 0;
            snip_logged = false;
        } else {
            if (!snip_since) snip_since = now ? now : 1;
            // Bounded: a capture UI that sits resident must not hide the overlay
            // for the rest of the session.
            if (now - snip_since < 5000) {
                if (!snip_logged) { snip_logged = true; Log("overlay hidden: capture UI active"); }
                return;
            }
        }
    }

    // A GDI capture of the game window is waiting for an overlay-free frame.
    // Skip drawing; Present still runs, so the frame it is waiting for arrives
    // immediately. (No-op when the protected output is in use: the game's back
    // buffer never holds the overlay in the first place.)
    if (!ShieldSurfaceReady() && g_clean_frame.Busy()) return;

    if (!BindSwapchain(sc)) return;

    if (ShieldSurfaceReady()) {
        // Follow the game's client area. Cheap: it only calls SetWindowPos when
        // the rectangle actually changed.
        comet_surface::Follow(g_surface, g_hwnd, g_client_w, g_client_h, ShieldVisible());
    } else {
        if (!g_rtv_live) CreateRTV();

        if (!g_rtv_live) return;
    }

    // Back-buffer size change -> rebuild the RTV and the ImGui device objects.
    // This must run *before* NewFrame: NewFrame uploads the font atlas using the
    // texture handle stored in io.Fonts->TexID, so starting a frame after
    // invalidating and before re-creating leaves that pointer dangling - a crash
    // or a blank atlas on a window resize or DPI change.
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        const bool sized = SUCCEEDED(sc->GetDesc(&desc)) &&
                           desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0;
        const bool resized = sized && (desc.BufferDesc.Width != g_width ||
                                       desc.BufferDesc.Height != g_height);
        if (resized || g_display_dirty) {
            g_display_dirty = false;
            ImGui_ImplDX11_InvalidateDeviceObjects();
            CreateRTV();
            ImGui_ImplDX11_CreateDeviceObjects();
            if (sized) {
                g_width = desc.BufferDesc.Width;
                g_height = desc.BufferDesc.Height;
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();

    io.MouseDrawCursor = Has(F_MENU_OPEN);
    if(Has(F_MENU_OPEN)) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        ClipCursor(nullptr);
    } else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    static bool previous_menu=false;
    if(previous_menu&&!Has(F_MENU_OPEN)) {io.ClearInputKeys();io.ClearInputMouse();}
    previous_menu=Has(F_MENU_OPEN);

    ImGui_ImplDX11_NewFrame();

    ImGui_ImplWin32_NewFrame();

    UpdateClientSize();

    if (g_client_w > 0 && g_client_h > 0) {

        ImVec2 ds((float)g_client_w, (float)g_client_h);

        if (io.DisplaySize.x != ds.x || io.DisplaySize.y != ds.y) {

            if (g_imgui_ready) g_reset_menu_pos = true;

        }

        io.DisplaySize = ds;

        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    }

    ImGui::NewFrame();

    // Resolve menu changes before choosing the visual destination in this same frame.
    auto* background=ImGui::GetBackgroundDrawList();
    background->ChannelsSplit(2);background->ChannelsSetCurrent(1);
    DrawMenu();
    EnsureWndProc();
    background->ChannelsSetCurrent(0);
    // Team numbering advances on every frame (not just when the ESP draws) so a
    // finished match clears the cache even if the ESP was off the whole time.
    g_team_labels.Frame(g_state.in_match && !g_ents.empty());
    DrawESP();DrawFOV();DrawRadar();
    background->ChannelsMerge();
    // Second check: a GDI capture can start while this frame is being built.
    if(g_unloading||(!ShieldSurfaceReady()&&g_clean_frame.Busy())) {ImGui::EndFrame();return;}

    ImGui::Render();

    // Cost split for the perf line: everything up to here is CPU work on the
    // game's render thread (ImGui frame + draw-list building for menu and ESP).
    {
        LARGE_INTEGER now_qpc{}, freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now_qpc);
        if (g_frame_start_qpc.QuadPart && freq.QuadPart)
            t_ui_ms = (double)(now_qpc.QuadPart - g_frame_start_qpc.QuadPart) * 1000.0 /
                      (double)freq.QuadPart;
    }

    // Protected output: the whole frame (menu, ESP, snaplines, radar, FOV) goes
    // to the capture-excluded window and the game's back buffer is not touched.
    // That is what keeps OBS Game Capture, Steam's F12 screenshot and any other
    // in-process Present hook from recording the overlay: they copy buffer 0,
    // and this frame never wrote to it.
    if (ShieldSurfaceReady()) {
        // Not on screen (game in the background, minimised, exclusive
        // fullscreen, or the snipping tool is up): draw nothing at all. Falling
        // back to the game's back buffer here would put the overlay into exactly
        // the buffer a capture reads, which is the thing this path exists to
        // prevent.
        if (!ShieldVisible()) return;

        LARGE_INTEGER freq{}, t0{}, t1{}, t2{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const bool shield_frame =
            comet_surface::Begin(g_surface, g_device, g_context, g_client_w, g_client_h);
        QueryPerformanceCounter(&t1);
        if (shield_frame) {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            comet_surface::End(g_surface, g_context);
            QueryPerformanceCounter(&t2);
            if (freq.QuadPart) {
                const double scale = 1000.0 / (double)freq.QuadPart;
                t_shield_begin_ms = (double)(t1.QuadPart - t0.QuadPart) * scale;
                t_shield_present_ms = (double)(t2.QuadPart - t1.QuadPart) * scale;
            }
            return;
        }
        // The surface refused this frame (resize race, device reset). Fall
        // through and draw in-game rather than dropping the frame silently.
    }

    // Save the game's pipeline state and put it back afterwards. Binding our RTV
    // without restoring the previous render target leaves the game drawing into
    // the wrong target with the wrong viewport on the next frame, which presents
    // as a frozen picture while the window still processes input.
    // Restore *exactly* what the game had bound. Asking for the full MRT set
    // matters: reading one target and rebinding one target would quietly drop
    // the rest of a multi-target set, and the count matters too - if the game
    // had nothing bound, rebinding "one target" is already wrong.
    constexpr UINT kMaxTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    ID3D11RenderTargetView* prev_rtvs[kMaxTargets]{};
    ID3D11DepthStencilView* prev_dsv = nullptr;
    g_context->OMGetRenderTargets(kMaxTargets, prev_rtvs, &prev_dsv);
    UINT prev_rtv_count = 0;
    while (prev_rtv_count < kMaxTargets && prev_rtvs[prev_rtv_count]) ++prev_rtv_count;

    D3D11_VIEWPORT prev_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vp_count = (UINT)(sizeof(prev_vp) / sizeof(prev_vp[0]));
    g_context->RSGetViewports(&vp_count, prev_vp);

    // Open a view on the back buffer for this draw only and release it before
    // Present. A render-target view left outstanding on buffer 0 keeps that
    // buffer un-releasable, so the game's wait for the previous buffer never
    // completes and the display stops advancing while frames keep being
    // produced. Unbind first, then release: the context holds its own reference
    // while a target is bound.
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    const bool have_back =
        SUCCEEDED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back;
    // Capture protection: keep a copy of the frame as the game made it, before a
    // single overlay pixel goes into this buffer. Done before the render target is
    // bound, so the back buffer is not read and written at the same time.
    if (have_back && CaptureProtectionOn()) {
        LARGE_INTEGER c0{}, c1{}, cf{};
        QueryPerformanceFrequency(&cf);
        QueryPerformanceCounter(&c0);
        RefreshCleanCopy(back);
        QueryPerformanceCounter(&c1);
        if (cf.QuadPart) t_copy_ms = (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / (double)cf.QuadPart;
    }
    if (have_back && SUCCEEDED(g_device->CreateRenderTargetView(back, nullptr, &rtv)) && rtv) {
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_context->OMSetRenderTargets(prev_rtv_count, prev_rtv_count ? prev_rtvs : nullptr, prev_dsv);
        rtv->Release();
    } else {
        // Could not reach the back buffer this frame: put the game's binding
        // back and draw nothing rather than leaving it changed.
        g_context->OMSetRenderTargets(prev_rtv_count, prev_rtv_count ? prev_rtvs : nullptr, prev_dsv);
    }
    if (back) back->Release();

    if (vp_count) g_context->RSSetViewports(vp_count, prev_vp);
    for (UINT i = 0; i < prev_rtv_count; ++i) prev_rtvs[i]->Release();
    if (prev_dsv) prev_dsv->Release();
}



// A swap chain whose Present starts failing produces exactly the symptom
// "the game runs, the screen does not move": frames are rendered and counted,
// but nothing is scanned out. Present's HRESULT used to be discarded here, so
// that state was invisible in the log. Record it, rate-limited so a persistent
// failure cannot flood the file.
static void NotePresentResult(HRESULT hr, const char* which) {
    static DWORD last_ms = 0;
    static int fails = 0;
    if (SUCCEEDED(hr)) return;
    ++fails;
    const DWORD now = GetTickCount();
    if (fails > 5 && now - last_ms < 1000) return;
    last_ms = now;
    Log("%s FAILED hr=%08x (failure %d)", which, (unsigned)hr, fails);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Log("%s: device lost, overlay drawing disabled", which);
        g_render_disabled = true;
    }
}

// Measure CPU time spent in overlay work and in the game's Present separately.
// This does not time GPU execution. Each presenting thread has its own sample;
// only the selected swap chain is recorded, and the log is written once per 10s.
struct PresentPerfSample {
    IDXGISwapChain* swapchain = nullptr;
    ULONGLONG started = 0;
    unsigned frames = 0, sync_zero = 0, sync_one = 0, sync_other = 0;
    double overlay_ms = 0.0, present_ms = 0.0, overlay_max_ms = 0.0;
    // Split of the overlay milliseconds, filled in by RenderFrame. `overlay_ms`
    // says how much the overlay costs; these say what it is spending it on.
    double pre_ms = 0.0;      // everything before the "anything to draw?" test
    double ui_ms = 0.0;       // ImGui frame + draw-list building (menu + ESP)
    double shield_begin_ms = 0.0;   // GetBuffer + RTV + state save
    double shield_present_ms = 0.0; // shield Present (blocking)
    double state_ms = 0.0;    // reading the runtime's state (shared memory or file)
    double copy_ms = 0.0;     // the clean-copy CopyResource
};


// What the game is actually presenting into, and what the panel is actually at.
//
// "60 in a match, 200+ in the range" is a display-mode question as often as a
// cost question: a game can put the panel into 60 Hz for a match and leave the
// desktop at 240, and then nothing the overlay does can change the number. This
// logs the swap chain mode and the desktop mode side by side, on rebind and
// whenever either changes, so that question is answered by the log instead of by
// a theory.
static void LogDisplayMode(bool force) {
    if (!g_swapchain) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(g_swapchain->GetDesc(&desc))) return;
    const DXGI_MODE_DESC& mode = desc.BufferDesc;
    unsigned chain_hz = 0;
    if (mode.RefreshRate.Denominator)
        chain_hz = (unsigned)((mode.RefreshRate.Numerator + mode.RefreshRate.Denominator / 2) /
                              mode.RefreshRate.Denominator);
    unsigned desk_hz = 0, desk_w = 0, desk_h = 0;
    DEVMODEA dm{};
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        desk_hz = dm.dmDisplayFrequency;
        desk_w = dm.dmPelsWidth;
        desk_h = dm.dmPelsHeight;
    }
    static unsigned last_chain = 0, last_desk = 0, last_w = 0, last_h = 0;
    static int last_windowed = -1;
    const int windowed = desc.Windowed ? 1 : 0;
    if (!force && chain_hz == last_chain && desk_hz == last_desk && mode.Width == last_w &&
        mode.Height == last_h && windowed == last_windowed)
        return;
    last_chain = chain_hz; last_desk = desk_hz; last_w = mode.Width; last_h = mode.Height;
    last_windowed = windowed;
    Log("[display] swapchain=%ux%u chain_hz=%u windowed=%d effect=%d flags=0x%x "
        "desktop=%ux%u desktop_hz=%u",
        (unsigned)mode.Width, (unsigned)mode.Height, chain_hz, windowed,
        (int)desc.SwapEffect, (unsigned)desc.Flags, desk_w, desk_h, desk_hz);
}

// The sync interval the overlay forwards. Normally the game's own value, untouched.
//
// Measured, not assumed: with the game asking for sync=1 the frame rate sat at 60
// while the overlay cost 0.013 ms per frame (ui_avg_ms=0.000), and the same
// session reached 226 fps the moment the game presented with sync=0. So when the
// frame rate is pinned, the game's own present call is the reason, and the only
// lever left on our side is this one - opt-in, because it tears and it defeats
// G-Sync/FreeSync.
static UINT EffectivePresentSync(UINT sync) {
    if (sync != 1 || !HasOpt2(OPT2_NO_VSYNC)) return sync;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("vsync override: presenting sync=0 (the game asked for 1)");
    }
    return 0;
}

static void NotePresentPerf(IDXGISwapChain* sc, const char* which, UINT sync,
                            const LARGE_INTEGER& begin, const LARGE_INTEGER& before_present,
                            const LARGE_INTEGER& after_present, const LARGE_INTEGER& frequency) {
    if (sc != g_swapchain || frequency.QuadPart <= 0) return;
    static thread_local PresentPerfSample sample;
    const ULONGLONG now = GetTickCount64();
    if (sample.swapchain != sc) {
        sample = PresentPerfSample{};
        sample.swapchain = sc;
        sample.started = now;
    }
    const double scale = 1000.0 / (double)frequency.QuadPart;
    const double render = (before_present.QuadPart - begin.QuadPart) * scale;
    const double present = (after_present.QuadPart - before_present.QuadPart) * scale;

    // Frame pacing for the Settings row and the periodic log line. Exponential
    // averages so one resize or load spike does not make the number useless.
    {
        static thread_local LARGE_INTEGER last_begin{};
        if (last_begin.QuadPart && begin.QuadPart > last_begin.QuadPart) {
            const double frame_s = (double)(begin.QuadPart - last_begin.QuadPart) /
                                   (double)frequency.QuadPart;
            if (frame_s > 0.0005 && frame_s < 1.0) {
                const double fps = 1.0 / frame_s;
                g_pacing.fps = g_pacing.fps > 0.0 ? g_pacing.fps * 0.9 + fps * 0.1 : fps;
            }
        }
        last_begin = begin;
        g_pacing.overlay_ms = g_pacing.overlay_ms > 0.0 ? g_pacing.overlay_ms * 0.9 + render * 0.1
                                                        : render;
        g_pacing.present_ms = g_pacing.present_ms > 0.0 ? g_pacing.present_ms * 0.9 + present * 0.1
                                                        : present;
        g_pacing.sync = sync;
        ++g_pacing.frames;
    }

    ++sample.frames;
    if (sync == 0) ++sample.sync_zero;
    else if (sync == 1) ++sample.sync_one;
    else ++sample.sync_other;
    sample.overlay_ms += render;
    sample.present_ms += present;
    sample.pre_ms += t_pre_ms;
    sample.ui_ms += t_ui_ms;
    sample.shield_begin_ms += t_shield_begin_ms;
    sample.shield_present_ms += t_shield_present_ms;
    sample.state_ms += t_state_ms;
    sample.copy_ms += t_copy_ms;
    if (render > sample.overlay_max_ms) sample.overlay_max_ms = render;
    if (now - sample.started >= 10000) {
        LogDisplayMode(false);
        // fps is measured between Present calls, so it is what the game actually
        // achieves; sync is what the engine asked for. sync=1 means the game is
        // vsync-bound and its ceiling is the display refresh rate, not the
        // overlay - that is the first thing to check when the frame rate looks
        // pinned at 60.
        // The scene is part of the measurement: fps alone cannot tell a heavy map
        // apart from our own per-entity cost, and the ESP is drawn per entity.
        Log("perf %s frames=%u interval_ms=%llu fps=%.1f sync0=%u sync1=%u sync_other=%u ents=%u menu=%d esp=%d overlay_avg_ms=%.3f overlay_max_ms=%.3f present_avg_ms=%.3f pre_avg_ms=%.3f ui_avg_ms=%.3f state_avg_ms=%.3f copy_avg_ms=%.3f shield_begin_ms=%.3f shield_present_ms=%.3f shield=%d chain=%s modules=%d",
            which, sample.frames, (unsigned long long)(now - sample.started), g_pacing.fps,
            sample.sync_zero, sample.sync_one, sample.sync_other,
            (unsigned)g_ents.size(), Has(F_MENU_OPEN) ? 1 : 0, Has(F_ESP) ? 1 : 0,
            sample.overlay_ms / sample.frames, sample.overlay_max_ms,
            sample.present_ms / sample.frames, sample.pre_ms / sample.frames,
            sample.ui_ms / sample.frames, sample.state_ms / sample.frames,
            sample.copy_ms / sample.frames, sample.shield_begin_ms / sample.frames,
            sample.shield_present_ms / sample.frames,
            ShieldSurfaceReady() ? 1 : 0,
            comet_shield::Describe(PresentChainPosition()), CaptureModules().Count());
        sample = PresentPerfSample{};
        sample.swapchain = sc;
        sample.started = now;
    }
}

// ---------------------------------------------------------------------------
// GDI capture detours (opt-in: Settings -> Protection -> "Delay GDI captures")
//
// A capture that names the game window (GetDC(game_hwnd) + BitBlt, PrintWindow,
// StretchBlt) reads whatever is in the window's surface at the moment it runs.
// While the overlay is drawn into the game's back buffer, the only honest way to
// hand such a capture a clean frame is to stop drawing and let one frame go out
// first. That is what CleanFrameScope does - it is the correct version of the
// "Sleep(2) and hope" that the public guides suggest, because it waits for a
// frame to actually be presented (bounded) instead of assuming a duration.
//
// Deliberately narrow:
//   * only BitBlt / StretchBlt / PrintWindow, not every GDI entry point;
//   * only when the source DC is the game window's DC, or the HWND is the game
//     window - a capture of another window, a memory DC or the desktop is left
//     completely alone;
//   * never when the protected output is active (nothing to hide);
//   * never from the render thread, where waiting would deadlock.
// ---------------------------------------------------------------------------
using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
using StretchBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
using PrintWindowFn = BOOL(WINAPI*)(HWND, HDC, UINT);

static BitBltFn g_bitblt = nullptr;
static StretchBltFn g_stretchblt = nullptr;
static PrintWindowFn g_printwindow = nullptr;

static bool GdiGuardArmed() {
    return HasOpt2(OPT2_GDI_GUARD) && !ShieldSurfaceReady() && g_hwnd && !g_unloading;
}

static bool CaptureReadsGameWindow(HDC source) {
    if (!source || !g_hwnd) return false;
    return WindowFromDC(source) == g_hwnd;
}

struct CleanFrameScope {
    bool armed = false;
    explicit CleanFrameScope(bool use) {
        if (!use) return;
        if (GetCurrentThreadId() == g_render_thread) return;  // would deadlock
        if (g_inside_present) return;
        armed = true;
        g_clean_frame.Request();
        const unsigned long long since = g_clean_frame.Frame();
        g_clean_frame.Await(since, comet_shield::kCaptureWaitMs,
                            [] { return GetTickCount64(); },
                            [] { SwitchToThread(); });
    }
    ~CleanFrameScope() { if (armed) g_clean_frame.Release(); }
};

static BOOL WINAPI HookBitBlt(HDC dst, int x, int y, int w, int h, HDC src,
                              int sx, int sy, DWORD rop) {
    CleanFrameScope scope(GdiGuardArmed() && CaptureReadsGameWindow(src));
    return g_bitblt(dst, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI HookStretchBlt(HDC dst, int x, int y, int w, int h, HDC src,
                                  int sx, int sy, int sw, int sh, DWORD rop) {
    CleanFrameScope scope(GdiGuardArmed() && CaptureReadsGameWindow(src));
    return g_stretchblt(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static BOOL WINAPI HookPrintWindow(HWND window, HDC dc, UINT flags) {
    CleanFrameScope scope(GdiGuardArmed() && window && window == g_hwnd);
    return g_printwindow(window, dc, flags);
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if(g_inside_present) return g_present(sc,sync,flags);
    PresentScope present_scope;
    if (!g_render_thread) g_render_thread = GetCurrentThreadId();
    if (g_unloading) {
        // The shield window belongs to this thread; tear it down here so the
        // window class and its WndProc cannot outlive the module.
        if (g_surface.state != comet_surface::State::Off) comet_surface::Destroy(g_surface);
        return g_present ? g_present(sc, sync, flags) : sc->Present(sync, flags);
    }

    LARGE_INTEGER perf_begin{}, perf_before{}, perf_after{};
    static thread_local LARGE_INTEGER perf_freq{};
    if (!perf_freq.QuadPart) QueryPerformanceFrequency(&perf_freq);
    QueryPerformanceCounter(&perf_begin);
    if (!g_render_disabled) {
        const unsigned fault = RenderFrameGuarded(sc);
        if (fault) NoteFault("present", fault);
    }
    // Log the game's requested sync interval once, without overriding it.
    static volatile LONG logged_game_present = 0;
    if (sc == g_swapchain && InterlockedCompareExchange(&logged_game_present, 1, 0) == 0) {
        Log("game Present sync=%u flags=0x%08x", (unsigned)sync, (unsigned)flags);
    }
    // Always forward, whatever happened above: skipping the game's own Present
    // is what turns an overlay fault into a frozen picture.
    QueryPerformanceCounter(&perf_before);
    const UINT out_sync = EffectivePresentSync(sync);
    const HRESULT hr = g_present(sc, out_sync, flags);
    QueryPerformanceCounter(&perf_after);
    // Releases a GDI capture that is waiting for an overlay-free frame (see the
    // PrintWindow/BitBlt detours below).
    g_clean_frame.OnPresented();
    NotePresentPerf(sc, "Present", out_sync, perf_begin, perf_before, perf_after, perf_freq);
    NotePresentResult(hr, "Present");
    return hr;
}





static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if(g_inside_present) return g_present1(sc,sync,flags,params);
    PresentScope present_scope;
    if (g_unloading) return g_present1 ? g_present1(sc, sync, flags, params) : sc->Present1(sync, flags, params);

    LARGE_INTEGER perf_begin{}, perf_before{}, perf_after{};
    static thread_local LARGE_INTEGER perf_freq{};
    if (!perf_freq.QuadPart) QueryPerformanceFrequency(&perf_freq);
    QueryPerformanceCounter(&perf_begin);
    if (!g_render_disabled) {
        const unsigned fault = RenderFrameGuarded(sc);
        if (fault) NoteFault("present1", fault);
    }
    static volatile LONG logged_game_present1 = 0;
    if (sc == g_swapchain && InterlockedCompareExchange(&logged_game_present1, 1, 0) == 0) {
        Log("game Present1 sync=%u flags=0x%08x", (unsigned)sync, (unsigned)flags);
    }
    QueryPerformanceCounter(&perf_before);
    const UINT out_sync = EffectivePresentSync(sync);
    const HRESULT hr = g_present1(sc, out_sync, flags, params);
    QueryPerformanceCounter(&perf_after);
    g_clean_frame.OnPresented();
    NotePresentPerf(sc, "Present1", out_sync, perf_begin, perf_before, perf_after, perf_freq);
    NotePresentResult(hr, "Present1");
    return hr;
}





static HRESULT ResizeBuffersBody(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {

    if (g_unloading) return g_resize ? g_resize(sc, count, w, h, fmt, flags) : sc->ResizeBuffers(count, w, h, fmt, flags);

    if (g_swapchain && sc != g_swapchain) {

        Log("resize skip other swapchain %p", sc);

        return g_resize(sc, count, w, h, fmt, flags);

    }

    // ResizeBuffers fails with DXGI_ERROR_INVALID_CALL while any view on the
    // back buffers is outstanding. Views are now per-frame and released before
    // Present, so nothing should be held here; clear the flag defensively and
    // re-probe below.
    g_rtv_live = false;

    if (g_imgui_ready) ImGui_ImplDX11_InvalidateDeviceObjects();

    HRESULT hr = g_resize(sc, count, w, h, fmt, flags);

    if (FAILED(hr)) Log("resize buffers failed %ux%u hr=%08x", w, h, (unsigned)hr);

    if (g_imgui_ready) {

        CreateRTV();

        // RenderFrame handles g_display_dirty before its next ImGui NewFrame.
        // Recreating here as well would compile shaders and recreate resources
        // twice for the same resize.

    }

    g_display_dirty = true;

    g_reset_menu_pos = true;

    Log("resize buffers %ux%u hr=%08x", w, h, (unsigned)hr);

    return hr;

}

// SEH wrapper: no destructible locals here (see the note above), so MSVC accepts
// the __try. A fault inside the resize path must not propagate into the game's
// ResizeBuffers caller, which usually aborts the process on failure.
static unsigned ResizeBuffersGuarded(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                     DXGI_FORMAT fmt, UINT flags, HRESULT& hr) {
    DWORD code = 0;
    COMET_TRY {
        hr = ResizeBuffersBody(sc, count, w, h, fmt, flags);
    }
    COMET_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
    }
    return (unsigned)code;
}

static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    HRESULT hr = S_OK;
    const unsigned fault = ResizeBuffersGuarded(sc, count, w, h, fmt, flags, hr);
    if (fault) {
        NoteFault("resize", fault);
        // Fall through to the original so the game still gets a well-formed
        // result instead of a made-up one.
        return g_resize ? g_resize(sc, count, w, h, fmt, flags) : hr;
    }
    return hr;
}




static bool InstallHook() {

    WNDCLASSEXW wc{};

    wc.cbSize = sizeof(wc);

    wc.lpfnWndProc = DefWindowProcW;

    wc.hInstance = GetModuleHandleW(nullptr);

    wc.lpszClassName = L"BS_OVERLAY_DUMMY";

    RegisterClassExW(&wc);

    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,

                             nullptr, nullptr, wc.hInstance, nullptr);

    if (!w) {

        Log("dummy window failed %lu", GetLastError());

        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        return false;

    }

    DXGI_SWAP_CHAIN_DESC sd{};

    sd.BufferCount = 1;

    sd.BufferDesc.Width = 8;

    sd.BufferDesc.Height = 8;

    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    sd.OutputWindow = w;

    sd.SampleDesc.Count = 1;

    sd.Windowed = TRUE;

    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr;

    ID3D11DeviceContext* ctx = nullptr;

    IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,

                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr)) {

        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,

                                           nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    }

    if (FAILED(hr) || !sc) {

        Log("dummy swapchain failed hr=%08x", (unsigned)hr);

        if (dev) dev->Release();

        if (ctx) ctx->Release();

        DestroyWindow(w);

        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        return false;

    }

    void** vt = *reinterpret_cast<void***>(sc);

    void* present_addr = vt[8];

    void* resize_addr = vt[13];

    IDXGISwapChain1* sc1 = nullptr;

    void* present1_addr = nullptr;

    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {

        void** vt1 = *reinterpret_cast<void***>(sc1);

        present1_addr = vt1[22];

    }

    MH_STATUS st = MH_Initialize();

    Log("MinHook init=%d present=%p present1=%p resize=%p", (int)st, present_addr, present1_addr, resize_addr);

    // Remember what we patched: a swap chain whose slot 8 still points here has
    // no other Present hook above ours (see PresentChainPosition).
    g_present_entry = present_addr;

    bool ok = true;

    if (MH_CreateHook(present_addr, (void*)&HookPresent, (void**)&g_present) != MH_OK) {

        ok = false;

        Log("MH_CreateHook present failed");

    }

    if (present1_addr && MH_CreateHook(present1_addr, (void*)&HookPresent1, (void**)&g_present1) != MH_OK) {

        Log("MH_CreateHook present1 failed");

    }

    if (resize_addr && MH_CreateHook(resize_addr, (void*)&HookResizeBuffers, (void**)&g_resize) != MH_OK) {

        Log("MH_CreateHook resize failed");

    }

    // Slot 9 is IDXGISwapChain::GetBuffer. Capture tools read the frame through
    // it; the detour answers them with the clean copy (see RefreshCleanCopy).
    if (vt[9] && MH_CreateHook(vt[9], (void*)&HookGetBuffer, (void**)&g_get_buffer) != MH_OK) {

        Log("MH_CreateHook GetBuffer failed");

        g_get_buffer = nullptr;

    }

    // GDI capture detours. Created unconditionally (the option is read later,
    // once the config is loaded) but they do nothing at all unless the user
    // turns "Delay GDI captures" on. A failure here must not take the overlay's
    // Present hook down with it.
    if (MH_CreateHook(reinterpret_cast<void*>(&BitBlt), reinterpret_cast<void*>(&HookBitBlt),
                      reinterpret_cast<void**>(&g_bitblt)) != MH_OK) {
        Log("MH_CreateHook BitBlt failed");
        g_bitblt = nullptr;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(&StretchBlt), reinterpret_cast<void*>(&HookStretchBlt),
                      reinterpret_cast<void**>(&g_stretchblt)) != MH_OK) {
        Log("MH_CreateHook StretchBlt failed");
        g_stretchblt = nullptr;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(&PrintWindow), reinterpret_cast<void*>(&HookPrintWindow),
                      reinterpret_cast<void**>(&g_printwindow)) != MH_OK) {
        Log("MH_CreateHook PrintWindow failed");
        g_printwindow = nullptr;
    }

    if (ok) {

        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {

            ok = false;

            Log("MH_EnableHook failed");

        }

    }

    if (sc1) sc1->Release();

    sc->Release();

    ctx->Release();

    dev->Release();

    DestroyWindow(w);

    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log("hook install %s", ok ? "ok" : "failed");

    return ok;

}



static DWORD WINAPI InitThread(LPVOID) {

    Sleep(800);

    InstallHook();

    return 0;

}



// ---------------------------------------------------------------------------
// Recoil Control System (RCS): mouse-assist layer. NOT no-recoil.
// No-recoil is handled by pyexec (bs_runtime.py): WeaponCase.GetRealRecoilData
// hooks + FpsPlacer impulse damping zero the pattern at source. RCS is a
// secondary user-driven mouse drag that ONLY runs when Strength > 0, meant
// as a backup if the memory hooks miss a weapon path.
// ---------------------------------------------------------------------------
static DWORD WINAPI RecoilControlSystemWorker(LPVOID) {
    // Pacing: a short 6 ms tick keeps drag smooth without thrashing SendInput.
    constexpr DWORD kActiveMs = 6;
    constexpr DWORD kIdleMs   = 200;   // flag/menu/match gate not active → cheap poll

    // Fractional carry so sub-pixel drag doesn't get truncated each tick.
    double carry = 0.0;
    ULONGLONG hold_start = 0;

    while (!g_unloading) {
        // Fast gate check first — skip the physical input polling 32× cheaper
        // when the subsystem is not engaged (menu open, out of match, strength=0).
        const bool engaged = Has(F_RECOIL) && !Has(F_MENU_OPEN) && g_state.in_match
                           && g_cfg.humanize > 0.0f;
        Sleep(engaged ? kActiveMs : kIdleMs);
        if (!engaged) { carry = 0.0; hold_start = 0; continue; }
        if (!g_hwnd) continue;

        HWND fg = GetForegroundWindow();
        if (fg != g_hwnd && GetAncestor(g_hwnd, GA_ROOT) != fg) {
            carry = 0.0; hold_start = 0; continue;
        }

        // RawAsyncKeyState reads the hardware state; IME overlays don't mask it.
        bool firing = (RawAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!firing) { carry = 0.0; hold_start = 0; continue; }

        // Warm-up: first ~70 ms of a burst has no visible kick, so skip it.
        ULONGLONG now = GetTickCount64();
        if (!hold_start) { hold_start = now; continue; }
        if (now - hold_start < 70) continue;

        // Drag per tick. Strength 0..2 maps to 0..~3 px / tick of downward offset.
        // Humanize flag adds ±0.5 px jitter so the compensation doesn't look robotic.
        float strength = std::clamp(g_cfg.humanize, 0.0f, 2.0f);
        double dy = strength * 1.5 * (kActiveMs / 6.0);
        if (Has(F_AIM_HUMANIZE)) {
            double j = ((double)(now & 0xFF) / 255.0 - 0.5);  // cheap deterministic jitter
            dy += j * 0.9;
        }
        carry += dy;
        int step = (int)carry;
        if (step == 0) continue;
        carry -= step;

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = 0;
        in.mi.dy = step;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));
    }
    return 0;
}





#ifndef CIYE_MENU_PREVIEW
// Set by CrashFilter install; see below.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_crash_filter = nullptr;
// Pre-resolved in DllMain, before anything can fault. The crash path must not
// touch the heap or the CRT file layer: if the fault happened with a heap or
// stdio lock held by this same thread, logging through them would hang the
// process instead of reporting the crash, and a hang looks exactly like the bug
// being diagnosed.
static wchar_t g_crash_log_path[MAX_PATH] = {};
static void CrashWrite(const char* text) {
    if (!g_crash_log_path[0] || !text || !*text) return;
    wchar_t wide[900];
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 900);
    if (n <= 1) return;
    HANDLE f = CreateFileW(g_crash_log_path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, wide, (DWORD)((n - 1) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);
}
#define CRASHLOG(...) do { \
    char _cl[800]; \
    _snprintf_s(_cl, sizeof(_cl), _TRUNCATE, __VA_ARGS__); \
    CrashWrite(_cl); \
} while (0)
// ---------------------------------------------------------------------------
// Crash forensics.
//
// A process that dies inside injected code leaves nothing behind on its own, so
// install a filter that records the exception before handing it on. That turns
// "it crashes a few seconds into a match" into a faulting module, an offset and
// a code - the difference between guessing and knowing.
// ---------------------------------------------------------------------------
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const void* where = rec->ExceptionAddress;
    HMODULE owner = nullptr;
    wchar_t owner_path[MAX_PATH]{};
    const wchar_t* owner_name = L"unknown";
    if (where && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(where), &owner) && owner) {
        if (GetModuleFileNameW(owner, owner_path, MAX_PATH)) {
            const wchar_t* slash = wcsrchr(owner_path, L'\\');
            owner_name = slash ? slash + 1 : owner_path;
        }
    }
    char module_ansi[MAX_PATH]{};
    WideCharToMultiByte(CP_ACP, 0, owner_name, -1, module_ansi, MAX_PATH, nullptr, nullptr);
    const unsigned long long offset =
        owner ? (unsigned long long)((uintptr_t)where - (uintptr_t)owner) : 0ull;
    CRASHLOG("CRASH code=%08x addr=%p module=%s +0x%llx params=%lu\n",
             (unsigned)rec->ExceptionCode, where, module_ansi, offset,
             (unsigned long)rec->NumberParameters);
    for (DWORD i = 0; i < rec->NumberParameters && i < 4; ++i) {
        CRASHLOG("CRASH param[%lu]=%p\n", (unsigned long)i,
                 reinterpret_cast<const void*>(rec->ExceptionInformation[i]));
    }
    CRASHLOG("CRASH state imgui=%d render_disabled=%d faults=%d in_present=%d\n",
             g_imgui_ready ? 1 : 0, g_render_disabled ? 1 : 0, g_fault_count,
             g_inside_present ? 1 : 0);
    // An excluded window is invisible to screen capture; never leave one behind.
    if (g_hwnd) SetWindowDisplayAffinity(g_hwnd, WDA_NONE);
    if (g_surface.window) SetWindowDisplayAffinity(g_surface.window, WDA_NONE);
    g_obs.active = false;
    g_obs.requested = false;
    // Chain to whoever was installed before us (the engine's own reporter), so
    // this is forensics, not a replacement. Returning EXECUTE_HANDLER here would
    // suppress the game's crash dialog.
    if (g_prev_crash_filter) return g_prev_crash_filter(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {

    if (reason == DLL_PROCESS_ATTACH) {

        g_hmodule = module;
        // Resolve the log path now, while allocating is still safe: the filter
        // must never allocate.
        {
            const std::wstring path = LogPath();
            const std::wstring::size_type n =
                path.size() < MAX_PATH - 1 ? path.size() : MAX_PATH - 1;
            for (std::wstring::size_type i = 0; i < n; ++i) g_crash_log_path[i] = path[i];
            g_crash_log_path[n] = L'\0';
        }

        // Must be armed before any other thread can fault.
        g_prev_crash_filter = SetUnhandledExceptionFilter(CrashFilter);

        DisableThreadLibraryCalls(module);

        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);

        if (t) CloseHandle(t);

        HANDLE r = CreateThread(nullptr, 0, RecoilControlSystemWorker, nullptr, 0, nullptr);

        if (r) CloseHandle(r);

    }

    return TRUE;

}
#endif
