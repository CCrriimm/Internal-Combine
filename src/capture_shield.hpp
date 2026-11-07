#pragma once
// Capture-shield policy core.
//
// WHY THIS FILE EXISTS
// --------------------
// The overlay draws into the game's DXGI back buffer, so anything that reads
// that buffer reads the overlay too. There are exactly three ways capture
// software can get at a frame, and they need different answers:
//
//   1. It reads the composed desktop / the window (DWM: Windows Graphics
//      Capture, desktop duplication, BitBlt of a window DC, PrintWindow).
//      -> WDA_EXCLUDEFROMCAPTURE (src/capture_guard.hpp) covers this. It is a
//         request to DWM; it has no effect on anything that reads a swap chain
//         directly.
//
//   2. It hooks IDXGISwapChain::Present inside this process and copies the
//      back buffer there (OBS "Game Capture", Steam F12, ShadowPlay, ReLive).
//      -> Nothing applied to a window can help. What helps is WHERE the hook
//         sits on the Present chain, and whether the overlay is in that buffer
//         at all. This file classifies the chain (see ClassifyPresentChain)
//         and the overlay's protected output (src/protected_output.hpp) keeps
//         the overlay out of the game's buffer entirely.
//
//   3. It asks this process for a frame through GDI (BitBlt/StretchBlt/
//      PrintWindow) or reads buffer 0 itself.
//      -> If the overlay is drawn into buffer 0, the only way to hand out a
//         clean frame is to not draw for the duration of the capture. That is
//         what CleanFrameGate does: it asks the render thread to stop drawing
//         and waits for one frame to actually be presented before the capture
//         proceeds. A blind Sleep() - as suggested in most of the guides -
//         either does nothing (the render thread may be mid-frame) or stalls
//         the game by however long the sleep is.
//
// This header is Win32-free on purpose so its decisions can be driven from a
// test with fake modules, fake clocks and no GPU.
//
// WHAT THIS FILE DOES NOT DO
// --------------------------
// It cannot make an overlay that is drawn into the game's back buffer invisible
// to a Present hook that copies *after* the draw. There is no flag, no call
// order and no return-address check that can: the pixels are already there and
// the pixels are exactly what the user is looking at. See
// docs/capture-protection.md.

#include <atomic>
#include <cstdint>

namespace comet_shield {

// ---------------------------------------------------------------------------
// In-process capture modules
// ---------------------------------------------------------------------------
// Names are matched against the module file name by the caller (overlay.cpp);
// this struct only records what was found so a decision can be made from it.
struct Modules {
    bool obs_graphics_hook = false;   // graphics-hook64.dll / graphics-hook32.dll
    bool obs_hook_dll      = false;   // hook64.dll / obs-gameapi.dll (compat mode)
    bool steam_overlay     = false;   // GameOverlayRenderer64.dll
    bool nvidia_share      = false;   // nvspcap64.dll / NVIDIA Overlay
    bool amd_relive        = false;   // AMDRSServ.dll / AMF runtime capture
    bool other_hook        = false;   // Bandicam, Fraps, Epic, Discord, ...

    int Count() const {
        return int(obs_graphics_hook) + int(obs_hook_dll) + int(steam_overlay) +
               int(nvidia_share) + int(amd_relive) + int(other_hook);
    }
    bool Any() const { return Count() > 0; }

    // True when at least one of the tools that copies the swap-chain back
    // buffer from inside the process is loaded.
    bool HooksBackBuffer() const {
        return obs_graphics_hook || obs_hook_dll || steam_overlay ||
               nvidia_share || amd_relive || other_hook;
    }
};

// Compact, allocation-free description for the log and the Settings row.
// Returns the number of bytes written (0 when nothing is loaded).
inline int Describe(const Modules& modules, char* out, int size) {
    if (!out || size <= 0) return 0;
    out[0] = '\0';
    int used = 0;
    auto add = [&](const char* text) {
        for (const char* p = text; *p && used + 1 < size; ++p) out[used++] = *p;
        out[used] = '\0';
    };
    auto sep = [&]() { if (used) add(", "); };
    if (modules.obs_graphics_hook) { sep(); add("OBS hook"); }
    if (modules.obs_hook_dll)      { sep(); add("OBS compat hook"); }
    if (modules.steam_overlay)     { sep(); add("Steam overlay"); }
    if (modules.nvidia_share)      { sep(); add("NVIDIA share"); }
    if (modules.amd_relive)        { sep(); add("AMD ReLive"); }
    if (modules.other_hook)        { sep(); add("other hook"); }
    return used;
}

// ---------------------------------------------------------------------------
// Present chain position
// ---------------------------------------------------------------------------
// Our Present detour is an inline patch of the real implementation entry point
// (the address a fresh swap chain's vtable slot 8 points at). Capture software
// normally hooks the *vtable slot* of the game's swap chain, which puts its
// detour above ours:
//
//     game -> [capture detour] -> copy back buffer -> [our detour] -> draw
//                                        ^ clean frame            ^ overlay
//
// so the copy does not contain the overlay for that frame. When nobody else is
// on the slot we are the first thing called, and every capture of the buffer
// that happens during or after our draw contains the overlay.
enum class Chain {
    Unknown = 0,    // no swap chain / no hook address to compare
    Topmost,        // slot == our entry point: we draw first, captures see it
    UnderCapturer,  // slot != our entry point: a detour above us copies first
};

inline Chain ClassifyPresentChain(const void* slot, const void* our_entry) {
    if (!slot || !our_entry) return Chain::Unknown;
    return slot == our_entry ? Chain::Topmost : Chain::UnderCapturer;
}

inline const char* Describe(Chain chain) {
    switch (chain) {
        case Chain::Topmost:       return "topmost Present hook (overlay is in the copied frame)";
        case Chain::UnderCapturer: return "capture hook above ours (copy happens before the draw)";
        default:                   return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Clean-frame gate
// ---------------------------------------------------------------------------
// Used by the GDI capture detours. A capture request makes the render thread
// skip drawing; the request completes only after one frame has actually been
// presented, so the DC the capturer is about to read holds game pixels only.
// The wait is bounded: if the render thread does not present (minimised game,
// paused engine, same-thread call) the capture proceeds anyway instead of
// hanging the caller.
class CleanFrameGate {
public:
    void Request() { pending_.fetch_add(1, std::memory_order_acq_rel); }
    void Release() {
        int current = pending_.load(std::memory_order_acquire);
        while (current > 0 &&
               !pending_.compare_exchange_weak(current, current - 1,
                                               std::memory_order_acq_rel)) {
        }
    }
    void OnPresented() { frames_.fetch_add(1, std::memory_order_release); }
    bool Busy() const { return pending_.load(std::memory_order_acquire) != 0; }
    unsigned long long Frame() const { return frames_.load(std::memory_order_acquire); }

    // Waits for a frame newer than `since`. `now_ms` and `yield` are injected so
    // the wait can be driven deterministically in a test. Returns true when a
    // new frame was seen.
    template <typename Clock, typename Yield>
    bool Await(unsigned long long since, unsigned timeout_ms, Clock now_ms, Yield yield) const {
        const unsigned long long start = now_ms();
        while (frames_.load(std::memory_order_acquire) == since) {
            if (now_ms() - start >= timeout_ms) return false;
            yield();
        }
        return true;
    }

private:
    std::atomic<unsigned long long> frames_{0};
    std::atomic<int> pending_{0};
};

// Default bound for a capture wait. Long enough for a 30 Hz frame plus jitter,
// short enough that a stuck render thread costs one capture, not the game.
inline constexpr unsigned kCaptureWaitMs = 40;

} // namespace comet_shield
