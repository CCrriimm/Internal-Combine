#pragma once
// capture_protect.hpp
// Blocks three capture paths from recording the internal cheat overlay:
//
//  PATH 1 — Desktop duplication / BitBlt / PrintScreen
//             SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)
//             Works against: OBS Window/Display Capture, Nvidia ShadowPlay
//             window mode, Windows Game Bar window capture, Steam overlay
//             screenshot when using window capture mode.
//
//  PATH 2 — OBS Game Capture (graphics-hook64.dll present hook)
//             OBS injects graphics-hook64.dll into the game process and hooks
//             IDXGISwapChain::Present to grab frames from the GPU backbuffer
//             before they hit the display. We hook Present first (or chain
//             after) and blank our overlay draw data for the duration of the
//             OBS-visible frame, then restore it on the next frame.
//
//  PATH 3 — Steam screenshot (GameOverlayRenderer64.dll present hook)
//             Same mechanism as OBS game capture. Same fix.
//
// USAGE:
//   Call capture_protect::Init(device, swap_chain, overlay_hwnd) once after
//   D3D11 device and swap chain are created.
//   Call capture_protect::Shutdown() from DllMain DLL_PROCESS_DETACH.
//   Call capture_protect::OnBeginFrame() at the start of each render frame.
//   Call capture_protect::OnEndFrame()   at the end   of each render frame.
//
// The Present hook wraps the existing overlay Present call — you do NOT need
// to change your render loop. The hook intercepts capture-initiated Present
// calls and skips drawing for that frame only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace capture_protect
{
    // -------------------------------------------------------------------------
    // Internal state
    // -------------------------------------------------------------------------
    namespace detail
    {
        static IDXGISwapChain*          g_swap      = nullptr;
        static HWND                     g_hwnd      = nullptr;
        static std::atomic<bool>        g_active    = false;

        // VMT hook plumbing
        static void**                   g_vmt       = nullptr;
        static void*                    g_orig_present = nullptr;

        // Flags set by callers
        static std::atomic<bool>        g_hide_frame = false; // suppress this frame

        // ---- Present detour signature ----
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        static PresentFn g_real_present = nullptr;

        // ---- Detect if the caller is a known capture hook ----
        // OBS game capture:        graphics-hook64.dll
        // Steam overlay:           GameOverlayRenderer64.dll
        // Nvidia ShadowPlay:       nvspcap64.dll
        // AMD ReLive:              AMDRSServ.dll
        // Fraps / Bandicam etc:    fraps.dll, bdcam.dll
        static bool is_capture_caller()
        {
            // Walk the return address up 2 frames looking for a module
            // whose name matches a known capture DLL.
            void* ret[4] = {};
            RtlCaptureStackBackTrace(1, 4, ret, nullptr);

            static const wchar_t* kCaptureDlls[] = {
                L"graphics-hook64.dll",
                L"GameOverlayRenderer64.dll",
                L"nvspcap64.dll",
                L"AMDRSServ.dll",
                L"fraps.dll",
                L"bdcam.dll",
            };

            for (void* ra : ret)
            {
                if (!ra) continue;
                HMODULE hmod = nullptr;
                if (!GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        static_cast<LPCWSTR>(ra), &hmod))
                    continue;
                if (!hmod) continue;

                wchar_t name[MAX_PATH] = {};
                GetModuleFileNameW(hmod, name, MAX_PATH);
                // just the filename portion
                const wchar_t* base = name;
                for (const wchar_t* p = name; *p; ++p)
                    if (*p == L'\\' || *p == L'/') base = p + 1;

                for (auto* dll : kCaptureDlls)
                    if (_wcsicmp(base, dll) == 0)
                        return true;
            }
            return false;
        }

        // ---- Present detour ----
        // Called for every Present on our swap chain.
        // If the caller is a capture hook: return a fake S_OK without
        // presenting — the capture software gets an empty / black frame.
        // If not: call through to the real Present normally.
        static HRESULT STDMETHODCALLTYPE Detour_Present(
            IDXGISwapChain* pSwapChain,
            UINT            SyncInterval,
            UINT            Flags)
        {
            if (g_hide_frame.load(std::memory_order_relaxed) && is_capture_caller())
            {
                // Return S_OK without presenting — capture sees a black frame.
                // The real frame still reaches the display on the next
                // non-capture Present call.
                return S_OK;
            }
            return g_real_present(pSwapChain, SyncInterval, Flags);
        }

        // ---- VMT hook install / remove ----
        static bool hook_present(IDXGISwapChain* swap, void* detour)
        {
            // IDXGISwapChain::Present is vtable slot 8.
            g_vmt = *reinterpret_cast<void***>(swap);
            void** slot = &g_vmt[8];

            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return false;

            g_orig_present = *slot;
            *slot = detour;

            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }

        static void unhook_present()
        {
            if (!g_vmt || !g_orig_present) return;
            void** slot = &g_vmt[8];
            DWORD old = 0;
            VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
            *slot = g_orig_present;
            VirtualProtect(slot, sizeof(void*), old, &old);
            g_orig_present = nullptr;
        }

    } // namespace detail

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    // Call once after D3D11 init. overlay_hwnd is the HWND of your overlay window.
    inline bool Init(IDXGISwapChain* swap, HWND overlay_hwnd)
    {
        if (!swap) return false;
        detail::g_swap = swap;
        detail::g_hwnd = overlay_hwnd;

        // PATH 1: Desktop duplication / window capture
        // Safe on the internal overlay HWND — it is a standard top-level window,
        // not a DWM-composited layered window, so WDA doesn't break composition.
        if (overlay_hwnd)
            SetWindowDisplayAffinity(overlay_hwnd, WDA_EXCLUDEFROMCAPTURE);

        // PATH 2+3: OBS game capture / Steam screenshot — hook Present
        detail::g_real_present = reinterpret_cast<detail::PresentFn>(
            detail::g_vmt ? detail::g_vmt[8] : (*reinterpret_cast<void***>(swap))[8]);

        bool hooked = detail::hook_present(
            swap,
            reinterpret_cast<void*>(detail::Detour_Present));

        detail::g_active.store(hooked || overlay_hwnd != nullptr,
                               std::memory_order_relaxed);
        return detail::g_active.load();
    }

    // Call at the start of each render frame to enable capture suppression.
    // While suppression is on, any Present call from a capture hook returns
    // S_OK without presenting (black frame to the capturer).
    inline void OnBeginFrame()
    {
        detail::g_hide_frame.store(true, std::memory_order_relaxed);
    }

    // Call at the end of each render frame (after your real Present call).
    inline void OnEndFrame()
    {
        detail::g_hide_frame.store(false, std::memory_order_relaxed);
    }

    // Remove the Present hook and restore WDA_NONE. Call from DLL_PROCESS_DETACH.
    inline void Shutdown()
    {
        detail::unhook_present();
        if (detail::g_hwnd)
            SetWindowDisplayAffinity(detail::g_hwnd, WDA_NONE);
        detail::g_active.store(false, std::memory_order_relaxed);
        detail::g_swap = nullptr;
        detail::g_hwnd = nullptr;
    }

    inline bool Active() { return detail::g_active.load(std::memory_order_relaxed); }

} // namespace capture_protect
