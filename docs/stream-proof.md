# Stream-proof

**Currently disabled at the user's request.** The configuration loader, publisher/saver and frame entry clear `F_STREAM_PROOF`; Settings shows it as temporarily disabled. No protected window is initialized during normal use. The complete menu/ESP frame renders directly to the game. The forced in-match null-cursor policy has been removed, so ESC/backtick and native game cursor selection are no longer overridden. Tests cover clearing an old saved enabled flag and publishing/saving it off. Restart the game before loading this build to remove the previous injected overlay and its window.

The following describes the dormant implementation, not an enabled feature.

Stream-proof keeps the menu, ESP, snaplines, radar and the aim FOV circle on a separate transparent D3D11/DirectComposition window. Only that window receives `WDA_EXCLUDEFROMCAPTURE`. The game window is reset to `WDA_NONE`, so enabling the feature does not exclude the game from capture.

Both Present hooks bypass the protected layer's own swap chain. One ImGui frame includes menu and visuals, then renders exclusively to the protected surface when Stream-proof is enabled. Existing game-window input handling remains active for the menu. The layer restores the original render target and follows the game client area. A window-thread timer hides it after focus loss/minimization even when the game stops presenting.

The surface uses `WS_EX_NOREDIRECTIONBITMAP`, premultiplied swap-chain alpha, and a transparent clear. It does not combine a layered GDI window and global alpha with DirectComposition. This removes the opaque redirection surface implicated by the reported black screen on focus. The surface is shown only after presentation and hidden while resizing. Microsoft describes this [composition-window pattern](https://learn.microsoft.com/en-us/archive/msdn-magazine/2014/june/windows-with-c-high-performance-window-layering-using-the-windows-composition-engine).

Use windowed or borderless mode. If capture exclusion or the protected renderer cannot initialize, visuals remain hidden rather than falling back to the captured game surface. Exclusive fullscreen is reported in Settings.

The native `bs_stream_proof_check` uses disposable off-screen windows. It checks capture affinity on both windows, composition/click-through styles, automatic hiding after focus loss, three initialize/render/resize/shutdown cycles, restoration of the primary render target/context, and that protected drawing leaves simulated game pixels unchanged. GPU readback checks empty pixels are alpha-zero while both ESP and menu pixels are present in the protected output. Radar and snapline geometry have separate native checks in the Python test suite.

These checks do not establish behavior in a live game or OBS recording. Actual OBS Game Capture, Window Capture and Display Capture still need verification on the user's capture setup. Windows capture exclusion has platform limitations and is not a security guarantee: [Microsoft documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity).

Artifact equip and gun kill-feed/deathbox work are deferred at the user's request. Artifact Apply remains disabled.

## Menu input and other overlays

The protected window accepts mouse hit tests and posts mouse events to the game's existing input handler in both menu states, converting client coordinates and preserving wheel screen coordinates. The game handler routes them to ImGui while the menu is open and to gameplay while it is closed. ImGui is never called from the protected window's message thread. Relying on `HTTRANSPARENT` alone failed across threads; Microsoft documents its [same-thread limitation](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-nchittest). Native checks cover down/up and wheel delivery in both states, coordinate conversion and activation of an actual ImGui button through the queued mouse-event path.

The menu uses the Windows cursor only. ImGui's software cursor and automatic OS cursor changes are disabled. Menu opening saves and releases the current cursor clip; closing restores that clip while the game remains foreground. Focus loss releases the saved state without trapping the cursor on the desktop. Button releases/capture-loss messages still reach ImGui after the menu closes, preventing a stuck pressed button on reopening. Real game cursor lock/visibility transitions remain to be verified.

ESP uses the foreground draw list so box, skeleton and labels draw above the menu like snaplines.

The game WndProc is installed once, rather than repeatedly reclaimed above hooks installed later by another overlay. Reclaiming could overwrite the previous-handler pointer and create a forwarding cycle. Present/Present1 share a thread-local reentry guard, and foreign swap chains are rejected before frame/input work. The protected window updates bounds only when they change, preserves Z order, and is shown only on visibility transitions rather than raised every frame. These address plausible interaction points with Discord; actual simultaneous Discord/game operation has not been independently verified.
