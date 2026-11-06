# Menu input

Stream-proof remains disabled. Menu and ESP render directly on the game surface.

While the menu is closed, the game window's original WndProc is restored. The overlay no longer hooks GetAsyncKeyState or GetKeyState. Thus keyboard, mouse and raw input use the native game path instead of passing through the menu handler. If a later overlay has chained our WndProc, it is not reclaimed or overwritten; the closed-menu handler transparently forwards to its saved predecessor.

Opening attaches the menu handler once. It captures keyboard only for text entry or active key binding, forwards releases, and distinguishes raw keyboard from raw mouse. Closing releases mouse capture only when acquired by the menu and clears ImGui's input state. The original handler address is checked before restoration to avoid overwriting another overlay.

The menu uses ImGui's drawn cursor with the Win32 backend hiding the OS cursor while it is open. Gameplay disables ImGui cursor changes. All custom ShowCursor adjustments, cursor-visibility forcing and saved-clip restoration have been removed. ClipCursor is released only while the menu is open; native gameplay chooses its cursor/locking behavior after closing.

Read-only diagnostics during investigation confirmed the running extracted DLL matched the previous build, menu and stream-proof flags were off, and the game owned foreground/focus. This ruled out an old DLL or a persistently open menu as the explanation, but does not independently establish the game's internal movement state.

The native check now sends messages through an actual disposable window's installed WndProc rather than directly invoking the hook. It verifies the original handler address after closing, repeated attach/detach cycles, movement key delivery, text/bind capture, release delivery and Escape/backtick behavior. The Python suite has 127 passing tests. Live game movement still requires verification.
