#pragma once
// Protected output: the only way an in-process overlay can be invisible to
// OBS "Game Capture" and to Steam's F12 screenshot.
//
// Both of those copy the game's swap-chain back buffer from inside the game
// process (graphics-hook64.dll / GameOverlayRenderer64.dll install their own
// Present detour). Whatever is in that buffer is what they record. So the
// overlay must not be in it: this window gets the drawing instead, and the
// window carries WDA_EXCLUDEFROMCAPTURE so the DWM-level paths (display
// capture, window capture, Game Bar, desktop duplication, BitBlt of the
// desktop) leave it out as well.
//
// Shape of the thing:
//
//   game window  ->  game swap chain   (never touched by the overlay)
//   shield window->  composition swap chain + DirectComposition visual
//                    premultiplied alpha -> transparent where nothing is drawn
//                    WDA_EXCLUDEFROMCAPTURE -> invisible to every capture path
//
// Requirements: Windows 8.1+, a D3D11 device created from a real adapter
// (WARP has no composition support) and windowed/borderless game output. In
// exclusive fullscreen the shield window is behind the game's output and the
// overlay is simply not visible; overlay.cpp checks for that and says so.
//
// Everything is best effort: if any step fails the caller keeps drawing into
// the game's back buffer exactly as before. Nothing here is required for the
// overlay to work.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <cstdint>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace comet_surface {

enum class State : int {
    Off = 0,     // never attempted (or released)
    Ready,       // window + swap chain + composition are live
    Failed,      // attempted and refused; `error` holds the code
};

inline constexpr wchar_t kWindowClass[] = L"CometCaptureShield";
inline constexpr int kMaxTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;

struct Output {
    State state = State::Off;
    HWND window = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;   // per frame, released in End
    ID3D11Texture2D* texture = nullptr;      // per frame, released in End
    IDCompositionDevice* compose = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    UINT width = 0;         // swap-chain size
    UINT height = 0;
    UINT win_w = 0;         // window size last applied
    UINT win_h = 0;
    int x = -100000, y = -100000;
    bool shown = false;
    bool excluded = false;      // WDA_EXCLUDEFROMCAPTURE was accepted
    long error = 0;             // first failing HRESULT / Win32 code

    // Pipeline state saved for the duration of one shield draw.
    ID3D11RenderTargetView* prev_rtv[kMaxTargets]{};
    ID3D11DepthStencilView* prev_dsv = nullptr;
    UINT prev_rtv_count = 0;
    D3D11_VIEWPORT prev_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT prev_vp_count = 0;
    bool bound = false;
};

namespace detail {

inline LRESULT CALLBACK WindowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        // WS_EX_TRANSPARENT already passes mouse input to the window below for
        // cross-thread hit testing; returning HTTRANSPARENT here keeps it
        // consistent for the same-thread case.
        case WM_NCHITTEST: return HTTRANSPARENT;
        // DestroyWindow must run on the owning thread. The unload path posts
        // this instead of calling DestroyWindow directly from another thread.
        case WM_CLOSE: DestroyWindow(h); return 0;
        default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

inline bool EnsureClass() {
    static int registered = -1;
    if (registered >= 0) return registered != 0;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    const ATOM atom = RegisterClassExW(&wc);
    registered = (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) ? 1 : 0;
    return registered != 0;
}

inline void ReleasePipeline(Output& out) {
    if (out.rtv) { out.rtv->Release(); out.rtv = nullptr; }
    if (out.texture) { out.texture->Release(); out.texture = nullptr; }
    if (out.swap) { out.swap->Release(); out.swap = nullptr; }
    if (out.visual) { out.visual->Release(); out.visual = nullptr; }
    if (out.target) { out.target->Release(); out.target = nullptr; }
    if (out.compose) { out.compose->Release(); out.compose = nullptr; }
}

inline void Fail(Output& out, long code) {
    ReleasePipeline(out);
    if (out.window && IsWindow(out.window)) {
        if (GetWindowThreadProcessId(out.window, nullptr) == GetCurrentThreadId())
            DestroyWindow(out.window);
        else
            SendMessageTimeoutW(out.window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
    }
    out.window = nullptr;
    out.state = State::Failed;
    out.error = code;
    out.shown = false;
    out.excluded = false;
    out.width = out.height = 0;
    out.win_w = out.win_h = 0;
}

} // namespace detail

// Creates the shield window, its composition swap chain and the DirectComposition
// tree. Returns true on success; on failure `state` is Failed and the caller
// should keep drawing into the game's back buffer.
inline bool Create(Output& out, ID3D11Device* device, HWND game, UINT width, UINT height) {
    if (out.state == State::Ready) return true;
    if (!device || !width || !height) { detail::Fail(out, E_INVALIDARG); return false; }
    if (!detail::EnsureClass()) { detail::Fail(out, (long)GetLastError()); return false; }

    // A fresh attempt: clear the previous failure code so the log is honest.
    out.error = 0;

    HWND w = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, L"", WS_POPUP, 0, 0, (int)width, (int)height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w) { detail::Fail(out, (long)GetLastError()); return false; }
    out.window = w;

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
    if (SUCCEEDED(hr) && dxgi_device) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr) && adapter) hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    if (SUCCEEDED(hr) && factory) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // required for composition
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // Three buffers instead of two: with two, GetBuffer(0) blocks until the
        // compositor has released the buffer we presented last frame, which puts a
        // compositor-sized stall inside the game's Present. The third buffer makes
        // that wait rare without skipping frames (a skipped frame leaves a stale
        // ESP on screen, which is what "trailing" looked like).
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_STRETCH;        // required for composition
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // required too
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = 0;
        hr = factory->CreateSwapChainForComposition(device, &desc, nullptr, &out.swap);
    }

    if (SUCCEEDED(hr) && out.swap && dxgi_device) {
        hr = DCompositionCreateDevice(dxgi_device, __uuidof(IDCompositionDevice),
                                      (void**)&out.compose);
    }
    if (SUCCEEDED(hr) && out.compose) hr = out.compose->CreateTargetForHwnd(w, TRUE, &out.target);
    if (SUCCEEDED(hr) && out.target) hr = out.compose->CreateVisual(&out.visual);
    if (SUCCEEDED(hr) && out.visual) hr = out.visual->SetContent(out.swap);
    if (SUCCEEDED(hr) && out.target) hr = out.target->SetRoot(out.visual);
    if (SUCCEEDED(hr) && out.compose) hr = out.compose->Commit();

    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgi_device) dxgi_device->Release();

    if (FAILED(hr)) { detail::Fail(out, (long)hr); return false; }

    out.width = width;
    out.height = height;
    out.state = State::Ready;
    // No display affinity on purpose.
    //
    // WDA_EXCLUDEFROMCAPTURE is the one flag that can hide this window from a
    // desktop capture, but DWM renders an excluded window as a black rectangle,
    // and this window covers the whole game: a Win+Shift+S snip then came back as
    // a black screen instead of a clean game frame. The captures that matter -
    // OBS Game Capture, Steam's F12, anything else that copies the game's swap
    // chain - never read this window anyway, so the flag bought nothing there and
    // cost a black screenshot. Instead the window hides itself while a screenshot
    // is being taken (SystemCaptureUiActive), which leaves the game visible with
    // no overlay and no black area.
    out.excluded = false;
    return true;
}

// Releases everything. Safe to call from any thread, any number of times.
// A window owned by the render thread is closed through WM_CLOSE so
// DestroyWindow runs on the thread that created it.
inline void Destroy(Output& out) {
    // Refuse while a frame is in flight. `out` holds the game's saved render
    // targets for the duration of Begin..End; releasing the swap chain and the
    // views here leaves the context bound to a dead target, the game renders into
    // it, and the restored state never happens. Callers defer to the top of the
    // next frame instead (overlay.cpp: MaintainShieldSurface).
    if (out.bound) return;
    detail::ReleasePipeline(out);
    if (out.window && IsWindow(out.window)) {
        SetWindowDisplayAffinity(out.window, WDA_NONE);
        if (GetWindowThreadProcessId(out.window, nullptr) == GetCurrentThreadId()) {
            DestroyWindow(out.window);
        } else {
            SendMessageTimeoutW(out.window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
            if (IsWindow(out.window)) {
                // The owner thread never pumped (the game stopped presenting).
                // Never leave a window whose WndProc lives in this module behind
                // an unload: give it the default procedure, then hide it. It is
                // destroyed with the process.
                SetWindowLongPtrW(out.window, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(DefWindowProcW));
                SetWindowDisplayAffinity(out.window, WDA_NONE);
                ShowWindow(out.window, SW_HIDE);
            }
        }
    }
    out.window = nullptr;
    out.state = State::Off;
    out.shown = false;
    out.excluded = false;
    out.width = out.height = 0;
    out.win_w = out.win_h = 0;
    out.x = out.y = -100000;
    out.bound = false;
    out.error = 0;
}

namespace detail {

inline bool IsCaptureHostName(const wchar_t* name) {
    static const wchar_t* kHosts[] = {
        L"ScreenClippingHost.exe",  // Win+Shift+S on Windows 11
        L"SnippingTool.exe",        // Windows 11 Snipping Tool
        L"ScreenSketch.exe",        // "Snipping Tool" package on 10/11
        L"GameBar.exe",             // Win+G / Win+Alt+R capture UI
        L"GameBarFTServer.exe",
    };
    if (!name) return false;
    for (const wchar_t* candidate : kHosts) {
        if (_wcsicmp(name, candidate) == 0) return true;
    }
    return false;
}

inline const wchar_t* ProcessBaseName(DWORD pid, wchar_t* buffer, DWORD size) {
    if (!pid || !buffer || !size) return nullptr;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return nullptr;
    DWORD written = size;
    const wchar_t* base = nullptr;
    if (QueryFullProcessImageNameW(process, 0, buffer, &written)) {
        base = buffer;
        for (const wchar_t* p = buffer; *p; ++p) {
            if (*p == L'\\' || *p == L'/') base = p + 1;
        }
    }
    CloseHandle(process);
    return base;
}

// A window belonging to one of the system's own screen-capture UIs.
inline bool IsCaptureHostWindow(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return false;
    wchar_t path[MAX_PATH] = {};
    const wchar_t* base = ProcessBaseName(pid, path, MAX_PATH);
    return base && IsCaptureHostName(base);
}

inline BOOL CALLBACK CaptureHostEnum(HWND window, LPARAM param) {
    if (!IsWindowVisible(window) || IsIconic(window)) return TRUE;
    if (!IsCaptureHostWindow(window)) return TRUE;
    // The snip overlay itself covers (nearly) the whole desktop. A smaller window
    // from the same executable only counts while it owns the foreground, which is
    // the state of the toolbox-less "new snip" popup.
    RECT rc{};
    if (!GetWindowRect(window, &rc)) return TRUE;
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0 || screen_h <= 0) return TRUE;
    const bool covers_desktop = (rc.right - rc.left) >= screen_w / 2 &&
                                (rc.bottom - rc.top) >= screen_h / 2;
    if (!covers_desktop && GetForegroundWindow() != window) return TRUE;
    *reinterpret_cast<HWND*>(param) = window;
    return FALSE;
}

// Win+Shift+S / PrtScn, read straight from the keyboard state.
//
// This is the part that makes a snip clean rather than black: the tool captures
// the composed desktop, so the window has to already be off screen when the
// selection is taken - waiting for the tool's own window to appear is one frame
// too late on some builds. Polling here means no hook and no dependency on which
// executable the build of Windows happens to use.
inline bool ScreenshotHotkeyRecentlyDown() {
    static ULONGLONG triggered_at = 0;
    const ULONGLONG now = GetTickCount64();
    const bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool s_key = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool prtsc = (GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) != 0;
    if ((win && shift && s_key) || prtsc) triggered_at = now;
    // Stay hidden through the selection itself and the tool's own capture.
    return triggered_at != 0 && now - triggered_at < 1500;
}

// ScreenClippingHost.exe only: it is spawned for the Win+Shift+S overlay and goes
// away again. SnippingTool.exe / GameBar.exe can sit resident, so those are only
// honoured through their windows (above) - hiding the overlay permanently because
// a helper process is parked in the tray would be worse than a stray ESP.
inline bool SnippingOverlayProcessRunning() {
    static ULONGLONG checked_at = 0;
    static bool running = false;
    const ULONGLONG now = GetTickCount64();
    if (checked_at && now - checked_at < 200) return running;
    checked_at = now ? now : 1;
    running = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ScreenClippingHost.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

} // namespace detail

// True while a screenshot of the desktop is being taken, in which case the shield
// hides itself instead of being captured.
//
// Three independent signals, because the tools differ: the hotkey itself (Win+
// Shift+S, PrtScn), the Windows 11 snip overlay process, and the capture UI
// windows of the snipping tool / Game Bar. Any of them hides the overlay, which is
// what makes a snip come back as the game with no ESP and no black rectangle.
inline bool SystemCaptureUiActive() {
    if (detail::ScreenshotHotkeyRecentlyDown()) return true;
    if (detail::SnippingOverlayProcessRunning()) return true;
    static ULONGLONG checked_at = 0;
    static bool active = false;
    const ULONGLONG now = GetTickCount64();
    if (checked_at && now - checked_at < 250) return active;
    checked_at = now ? now : 1;
    HWND found = nullptr;
    EnumWindows(detail::CaptureHostEnum, reinterpret_cast<LPARAM>(&found));
    active = found != nullptr;
    return active;
}

// True when the shield window should be on screen: the game is the foreground
// window and is not minimised. Exclusive fullscreen is rejected earlier, when
// the surface is created (overlay.cpp: GameUsesWindowedPresentation).
inline bool ShouldShow(HWND game) {
    if (!game || !IsWindow(game) || !IsWindowVisible(game) || IsIconic(game)) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg != game && GetAncestor(game, GA_ROOT) != fg) return false;
    return true;
}

// Keeps the window over the game's client area. Only repositions on change, so
// this is cheap enough to call every frame.
inline void Follow(Output& out, HWND game, UINT width, UINT height, bool visible) {
    if (out.state != State::Ready || !out.window || !game) return;

    if (!visible) {
        if (out.shown) { ShowWindow(out.window, SW_HIDE); out.shown = false; }
        return;
    }

    RECT rc{};
    if (!GetClientRect(game, &rc)) return;
    POINT top_left{0, 0};
    if (!ClientToScreen(game, &top_left)) return;
    const int x = top_left.x;
    const int y = top_left.y;
    const UINT w = width ? width : (UINT)(rc.right - rc.left);
    const UINT h = height ? height : (UINT)(rc.bottom - rc.top);
    if (!w || !h) return;

    if (x != out.x || y != out.y || w != out.win_w || h != out.win_h || !out.shown) {
        SetWindowPos(out.window, HWND_TOPMOST, x, y, (int)w, (int)h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        out.x = x;
        out.y = y;
        out.win_w = w;
        out.win_h = h;
        out.shown = true;
    }
}

inline void Hide(Output& out) {
    if (out.state == State::Ready && out.window && out.shown) {
        ShowWindow(out.window, SW_HIDE);
        out.shown = false;
    }
}

// Acquires this frame's back buffer, binds it and saves the game's pipeline
// state. Returns false when the target is not usable this frame (the caller then
// falls back to the game's back buffer).
//
// The view is acquired and released per frame on purpose. This is a flip-model
// swap chain, so buffer 0 is recycled into a different buffer on every Present;
// holding a render-target view on it keeps that buffer un-releasable and the
// swap chain stops rotating - the frozen-picture failure this project already
// hit once on the game's own swap chain (see CreateRTV in overlay.cpp).
inline bool Begin(Output& out, ID3D11Device* device, ID3D11DeviceContext* ctx,
                  UINT width, UINT height) {
    if (out.state != State::Ready || !out.swap || !ctx || !device) return false;
    if (!width || !height || out.bound) return false;

    if (out.width != width || out.height != height) {
        HRESULT hr = out.swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { out.error = (long)hr; return false; }
        out.width = width;
        out.height = height;
    }

    ID3D11Texture2D* back = nullptr;
    HRESULT hr = out.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (FAILED(hr) || !back) { out.error = (long)hr; return false; }
    hr = device->CreateRenderTargetView(back, nullptr, &out.rtv);
    if (FAILED(hr) || !out.rtv) {
        back->Release();
        out.rtv = nullptr;
        out.error = (long)hr;
        return false;
    }
    out.texture = back;   // released in End, after the view is unbound

    // Save the game's pipeline state, exactly as the in-game path does: the
    // engine keeps drawing after Present returns.
    out.prev_rtv_count = kMaxTargets;
    ctx->OMGetRenderTargets(kMaxTargets, out.prev_rtv, &out.prev_dsv);
    while (out.prev_rtv_count > 0 && !out.prev_rtv[out.prev_rtv_count - 1])
        --out.prev_rtv_count;
    out.prev_vp_count = (UINT)(sizeof(out.prev_vp) / sizeof(out.prev_vp[0]));
    ctx->RSGetViewports(&out.prev_vp_count, out.prev_vp);

    ctx->OMSetRenderTargets(1, &out.rtv, nullptr);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // premultiplied: fully transparent
    ctx->ClearRenderTargetView(out.rtv, clear);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    out.bound = true;
    return true;
}

// Releases the frame's back buffer, puts the game's pipeline state back and
// presents the shield surface.
inline bool End(Output& out, ID3D11DeviceContext* ctx) {
    if (!out.bound) return false;
    out.bound = false;

    // Unbind before releasing: the context holds its own reference while a
    // target is bound, and Present needs the buffer free.
    if (ctx) {
        ctx->OMSetRenderTargets(out.prev_rtv_count,
                                out.prev_rtv_count ? out.prev_rtv : nullptr, out.prev_dsv);
        if (out.prev_vp_count) ctx->RSSetViewports(out.prev_vp_count, out.prev_vp);
    }
    for (UINT i = 0; i < out.prev_rtv_count; ++i) out.prev_rtv[i]->Release();
    for (UINT i = 0; i < kMaxTargets; ++i) out.prev_rtv[i] = nullptr;
    if (out.prev_dsv) { out.prev_dsv->Release(); out.prev_dsv = nullptr; }
    out.prev_rtv_count = 0;
    out.prev_vp_count = 0;
    if (out.rtv) { out.rtv->Release(); out.rtv = nullptr; }
    if (out.texture) { out.texture->Release(); out.texture = nullptr; }

    const HRESULT hr = out.swap ? out.swap->Present(0, 0) : E_FAIL;
    if (FAILED(hr)) {
        out.error = (long)hr;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            // The device is gone; the game will handle it. Stop using the
            // surface so the caller falls back to drawing in-game.
            out.state = State::Failed;
            return false;
        }
    }
    return true;
}

inline const char* Describe(const Output& out, char* buffer, int size) {
    if (!buffer || size <= 0) return "";
    switch (out.state) {
        case State::Ready:
            _snprintf_s(buffer, (size_t)size, _TRUNCATE,
                        "protected output %ux%u", out.width, out.height);
            return buffer;
        case State::Failed:
            _snprintf_s(buffer, (size_t)size, _TRUNCATE,
                        "protected output unavailable (0x%08lX)", (unsigned long)out.error);
            return buffer;
        default:
            return "protected output off";
    }
}

} // namespace comet_surface
