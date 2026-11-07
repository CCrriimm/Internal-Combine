# Capture protection

What the overlay can and cannot hide, path by path, and what this build actually
implements. Read the first table before changing anything here.

## The three capture paths

| Path | Who does it | What it reads | What stops it |
|---|---|---|---|
| **Composed desktop / window** | OBS *Window* and *Display* capture (BitBlt or Windows Graphics Capture), Windows Game Bar, desktop duplication, PrintWindow | The DWM output, i.e. what the monitor shows | `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` on the window that must not appear, plus hiding it while the snipping tool is open |
| **Swap-chain hook** | OBS *Game Capture* (`graphics-hook64.dll`), Steam F12 (`GameOverlayRenderer64.dll`), ShadowPlay (`nvspcap64.dll`), ReLive | The game's DXGI back buffer, copied inside the game process by a `Present` detour | The overlay must not be in that buffer. Window flags do nothing here |
| **GDI read of the game window** | Anti-cheat screenshot modules, older tools | `BitBlt`/`StretchBlt`/`PrintWindow` on a DC of the game window | Nothing, once the pixels are drawn. Drawing must be held back for that capture |

The three need three different mechanisms, and the middle row is the one that
matters for OBS Game Capture and Steam.

## Why the usual advice does not work

Every guide on this subject (and every chat-model answer that repeats them)
suggests one of these. Each one is checked against the code in this repository.

**"`SetWindowDisplayAffinity` also covers OBS Game Capture."** It does not.
Window affinity is a DWM request that affects composition-time capture. A
recorder with a `Present` detour inside the game process never asks DWM for
anything: it has the back buffer, and the back buffer contains the overlay
because the overlay draws into it (`RenderFrame` in `src/overlay.cpp`). This is
why `Settings -> Windows capture exclusion` reports "active" while OBS still
records the ESP.

**"Return `S_OK` from your `Present` hook without presenting, and the capturer
gets a black frame."** The capture already happened. OBS and Steam hook the swap
chain's vtable, which places their detour *above* the implementation entry point
that MinHook patches:

```
game -> [OBS detour: copies buffer 0] -> [our detour: draws the overlay] -> Present
```

Skipping the forward pass at our level changes nothing for them, and it freezes
the user's screen for that frame. There is no flag, no return value and no call
order at our level that un-draws pixels that are already in the buffer.

**"Check `_ReturnAddress()`/the call stack for `graphics-hook64.dll`."** When our
detour runs, the immediate caller is the game's own render thread (`Present` is
called from `CDXGISwapChain::Present`'s caller or from OBS's detour - either way
the frame is not ours to inspect). Stack walking tells you which module *called
Present*, not who is going to copy the buffer afterwards, and the answer for OBS
is "the game". `capture_protect.hpp` from the guides cannot work for this reason,
on top of not compiling (`static_cast<LPCWSTR>(void*)` is not a valid cast) and
hooking slot 8 of a swap chain the overlay does not own.

**"Hook `BitBlt` with MinHook and `Sleep(2)`."** A sleep inside the capture hook
races the render thread: if the frame was already drawn, sleeping changes
nothing; if it was not, the sleep ends before the next frame arrives. What is
needed is a *frame*, not a duration - see `CleanFrameGate` in
`src/capture_shield.hpp`. This build implements the correct version and leaves it
**off by default**, because an unconditional `BitBlt` detour also sits in front of
OBS Window Capture, the Steam overlay and Discord, and every one of those would
then wait for a frame it does not need.

**"`WDA_EXCLUDEFROMCAPTURE` is free."** It is not. To leave a window out of the
capture buffer, DWM has to compose that window instead of letting it bypass
composition, so a flip-model game loses independent flip while affinity is set.
On a 60 Hz desktop that shows up as a hard 60 fps ceiling. That is the first
thing to look at when the frame rate looks pinned - the `perf` line in
`overlay_log.txt` now carries `fps=` and `sync=` so the two can be told apart
(`sync=1` means the engine asked for vsync and the ceiling is the display, not
the overlay).

## What this build does

Three independent switches in `Settings -> Protection`.

### Protected output (default on)

`src/protected_output.hpp` creates a top-level `WS_EX_NOREDIRECTIONBITMAP`
window, a `CreateSwapChainForComposition` swap chain with premultiplied alpha,
and a DirectComposition visual tree. Each frame that has something to draw
(menu, ESP, snaplines, radar, FOV circle) is rendered there and the game's back
buffer is never written to.

That is what makes OBS Game Capture and Steam's F12 clean: they copy buffer 0 of
the game's swap chain, and buffer 0 no longer contains anything the overlay drew.

The shield window deliberately carries **no** display affinity.

`WDA_EXCLUDEFROMCAPTURE` is the only flag that hides a window from a *desktop*
capture, but DWM then renders that window as a black rectangle - and this window
covers the whole game, so a `Win+Shift+S` snip came back as a black screen instead
of a clean game frame. It also bought nothing where it mattered: OBS Game Capture
and Steam copy the game's swap chain, not the composed desktop.

Instead the window hides itself (`SystemCaptureUiActive()`) whenever a screenshot
is being taken, which leaves the game visible with no overlay and no black area:

* the hotkey itself - `Win+Shift+S` and `PrtScn` are read from the keyboard state
  each frame, so the window is gone before the tool grabs the composed desktop;
* `ScreenClippingHost.exe`, the process Windows 11 spawns for the snip overlay;
* the capture-UI windows of the snipping tool / Game Bar, including the small
  "new snip" popup once it owns the foreground.

Nothing is blanked on purpose, and the game window itself is not excluded either
(that is the other way to turn a screenshot black).

Presenting is blocking (`Present(0, 0)`). A non-blocking present was tried and
withdrawn: a skipped frame leaves the previous ESP frame on screen, which reads as
the overlay trailing the world while the camera turns.

### Retired: Windows capture exclusion (game window)

The game window used to carry `WDA_EXCLUDEFROMCAPTURE` with a re-apply loop. That
is gone: it hid the game itself from Windows capture, which paints the *whole
game* black in a screenshot or a display capture, and it can drop a flip-model
game off independent flip. `ApplyObsProtection()` now always applies `false`,
which also clears an exclusion left behind by an older config.

### Retired: Delay GDI captures

Hooks on `BitBlt`, `StretchBlt` and `PrintWindow`; off by default, and not needed
while the protected output is active, so the switch is no longer shown.

### Delay GDI captures (off by default)

Hooks `BitBlt`, `StretchBlt` and `PrintWindow` with MinHook. A call is only
delayed when the source DC belongs to the game window (`WindowFromDC`) or the
`HWND` is the game window; captures of other windows, memory DCs and the desktop
are untouched. The delay is `CleanFrameGate`: the request makes `RenderFrame`
skip drawing, and the call proceeds once `Present` has actually run (bounded by
`kCaptureWaitMs`, 40 ms). It never waits when it is called from the render thread
or from inside `Present`, where waiting could not succeed.

This only has meaning while the overlay is drawn into the game's back buffer. It
does nothing for OBS Game Capture or Steam, and it is inactive while the
protected output is on.

## Limits that no amount of code changes

* Hardware capture (a capture card, an HDMI splitter) records the monitor output.
  Nothing in the process can see it.
* An anti-cheat that reads the game's back buffer **inside the engine's own
  Present**, before our detour runs, sees the previous frame's contents, which is
  the game only - that is the same guarantee OBS gets. An anti-cheat that reads
  the swap chain from another thread at an arbitrary moment can catch a frame the
  overlay drew when the protected output is off; with it on, buffer 0 is never
  written by the overlay at all.
* A capture tool that hooks the *shield* window instead of the game will get a
  transparent surface, which is the intended result.
* `WDA_EXCLUDEFROMCAPTURE` is a request. A recorder that ignores display affinity
  is not stopped by it.

## Frame rate

Three different things can pin the frame rate, and they have different answers.
`Settings -> Protection` shows all the numbers live; `overlay_log.txt` repeats
them every 10 s.

1. **`sync=1` in the pacing line.** The engine asked for vsync, so the ceiling is
   the display refresh rate (60 on a 60 Hz panel) whatever the overlay does.
   Nothing in this project changes the `SyncInterval` the game passes to
   `Present` - it is forwarded untouched - so this is the game's own setting.
2. **`overlay_avg_ms` is a large part of the frame budget.** That is overlay work
   (ImGui frame, ESP draw, state poll) and is worth reporting with a match state,
   entity count and resolution.
3. **Neither, and the frame rate is still capped at the desktop refresh.** That is
   the composition ceiling: `WDA_EXCLUDEFROMCAPTURE` on the game window (and a
   topmost overlay over a full-screen game) makes DWM compose the frame instead of
   letting the game bypass composition. Toggling `Windows capture exclusion (game
   window)` off while `Protected output` stays on is the A/B test - the protected
   output already covers the OBS/Steam paths, so if the numbers improve with the
   game-window setting off, leave it off.

The protected output draws only on frames that have something to draw and hides
its window otherwise, so the game returns to its normal presentation path when
the menu is closed and the ESP is off.

## Verifying it

`overlay_log.txt` next to the DLL carries, once per 10 s:

```
perf Present frames=... fps=... sync0=... sync1=... overlay_avg_ms=... shield=1 chain=... modules=...
capture shield ready: protected output 2560x1440, capture exclusion on
capture modules: OBS hook, Steam overlay
```

`shield=1` means the protected output is live and the game's back buffer is not
being drawn into. `chain=topmost ...` means no other `Present` hook is above ours
(see `ClassifyPresentChain`); `chain=capture hook above ours ...` means a
recorder's detour is above ours, which is the normal case and the good one for
in-process drawing. `modules=` lists the capture tools currently loaded.

`Settings -> Protection` shows the same information live, plus the frame pacing
(`fps`, overlay ms, present ms, sync) that tells an overlay cost apart from a
vsync or composition ceiling.
