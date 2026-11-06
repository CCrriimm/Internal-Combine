// OBS / screenshot capture protection for the game window.
//
// Ported from the reference build (deps/window/capture_blackout.hpp) and kept
// header-only so it can be exercised without a D3D device.
//
//   SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)
//
// asks Windows capture paths (OBS Game/Window Capture, Snipping Tool, the
// desktop compositor's capture surface, ...) to exclude the window. It is
// best-effort by design: hardware capture and recorders that ignore display
// affinity still see the pixels, so the manual hide key remains available and
// the UI reports the real API result rather than claiming success.
//
// The "bypass" part is the re-apply loop: the game (or anything it hands the
// window to) can clear or reject the affinity at any time, so while protection
// is requested the guard polls GetWindowDisplayAffinity and re-asserts it when
// it was dropped. All work is timer-gated - a failed call backs off for two
// seconds instead of being retried every frame, and a confirmed affinity is
// only re-checked twice a second.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#include <cstdint>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace comet_capture {

enum class Status : int {
    Off = 0,      // not requested
    Active,       // affinity confirmed applied
    Denied,       // the API refused; `error` holds the Win32 error
    WindowLost,   // no usable window (game closing / recreating it)
};

inline const char* StatusName(Status s) {
    switch (s) {
        case Status::Active: return "active";
        case Status::Denied: return "denied";
        case Status::WindowLost: return "no window";
        default: return "off";
    }
}

struct Blackout {
    HWND window = nullptr;
    bool requested = false;   // what the user (or the default) asked for
    bool attempted = false;
    bool succeeded = false;
    bool active = false;      // affinity is believed to be in force
    unsigned long error = 0;
    std::uint64_t last_attempt = 0;
    std::uint64_t last_verify = 0;
    int reapplies = 0;        // how many times the game/OS cleared it
    Status status = Status::Off;
};

namespace detail {
inline constexpr std::uint64_t kRetryMs = 2000;  // backoff after a failure
inline constexpr std::uint64_t kVerifyMs = 500;  // affinity poll cadence
} // namespace detail

// Applies, restores or re-asserts display affinity. Safe to call every frame.
// `now_ms` is GetTickCount64() in production and an explicit counter in tests.
inline void Apply(HWND window, bool enable, Blackout& state, std::uint64_t now_ms) {
    if (!window || !IsWindow(window)) {
        if (state.requested || state.active) {
            // `requested` is deliberately kept: a recreated game window is
            // protected again on the next call.
            state.active = false;
            state.succeeded = false;
            state.window = nullptr;
            state.status = Status::WindowLost;
        }
        return;
    }

    const bool window_changed = state.window != window;
    if (window_changed) {
        state.active = false;
        state.succeeded = false;
        state.attempted = false;
        state.error = 0;
        state.reapplies = 0;
        state.status = enable ? Status::Off : state.status;
    }

    state.window = window;
    state.requested = enable;

    if (!enable) {
        // Restore once; then stay quiet until something asks again.
        const bool pending = state.active || (state.attempted && state.status == Status::Denied);
        if (!pending) {
            state.status = Status::Off;
            return;
        }
        if (state.attempted && state.status == Status::Denied &&
            now_ms - state.last_attempt < detail::kRetryMs)
            return;

        state.attempted = true;
        state.last_attempt = now_ms;
        SetLastError(ERROR_SUCCESS);
        const BOOL ok = SetWindowDisplayAffinity(window, WDA_NONE);
        state.succeeded = ok != FALSE;
        state.error = ok ? 0 : GetLastError();
        if (ok) {
            state.active = false;
            state.error = 0;
            state.status = Status::Off;
        } else {
            // The exclusion may still be in force; never claim it was removed.
            state.status = Status::Denied;
        }
        return;
    }

    if (state.succeeded && !window_changed) {
        // Confirmed earlier. Re-check occasionally in case the game cleared it.
        if (now_ms - state.last_verify < detail::kVerifyMs) return;
        state.last_verify = now_ms;
        DWORD current = WDA_NONE;
        if (GetWindowDisplayAffinity(window, &current) && current == WDA_EXCLUDEFROMCAPTURE) {
            state.active = true;
            state.status = Status::Active;
            return;
        }
        // Dropped by someone else - re-assert below and count it.
        ++state.reapplies;
        state.succeeded = false;
    } else if (state.attempted && now_ms - state.last_attempt < detail::kRetryMs) {
        // Bounded retry after a failure; keep the previous status truthful.
        if (state.status != Status::Active) state.status = Status::Denied;
        return;
    }

    state.attempted = true;
    state.last_attempt = now_ms;
    state.last_verify = now_ms;

    SetLastError(ERROR_SUCCESS);
    const BOOL ok = SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    state.succeeded = ok != FALSE;
    if (ok) {
        state.active = true;
        state.error = 0;
        state.status = Status::Active;
        return;
    }

    state.error = GetLastError();
    if (state.error == ERROR_SUCCESS) state.error = ERROR_GEN_FAILURE;
    state.active = false;
    state.status = Status::Denied;
}

// Panic/shutdown path: always attempts the restore once, ignoring the timers.
inline void Restore(HWND window, Blackout& state) {
    state.requested = false;
    state.attempted = false;
    state.status = Status::Off;
    if (window && IsWindow(window)) {
        SetWindowDisplayAffinity(window, WDA_NONE);
    }
    state.active = false;
    state.succeeded = false;
}

inline const char* Describe(const Blackout& state) {
    switch (state.status) {
        case Status::Active:
            return state.reapplies > 0 ? "active (re-applied)" : "active";
        case Status::Denied:
            return "SetWindowDisplayAffinity failed";
        case Status::WindowLost:
            return "game window unavailable";
        default:
            return "off";
    }
}

} // namespace comet_capture
