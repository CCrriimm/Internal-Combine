# ImGui in-game menu

The overlay renders the menu with ImGui and the existing Direct3D 11 swap chain. Menu34 initialization and rendering are removed from the overlay; only the loader retains its D2D/DirectWrite implementation. Overlay imports were checked after a Release build: no d2d1.dll or dwrite.dll dependency.

`src/menu_theme.h` isolates the menu's cyan/slate palette, controls and motion settings from the loader's `src/ui_theme.h`. The main window uses a 188px rail, a 960×680 default size, independent tab-body scrolling and two-column content groups. All existing feature flags and saved controls remain bound to `g_cfg`; accent and Reduced Motion preferences are restored directly into ImGui. Settings and the header expose Save. Insert is labeled correctly rather than Num 0.

Build checks: 109 offline tests pass. `BS_MENU_PREVIEW=ON` enables `bs_menu_preview`, an isolated D3D11 WARP renderer that captures the actual menu source. It never starts game hooks, never loads user config, uses fixture skin data, and disables config saving. Eight images cover Visuals, Aim, Skins and Settings at normal and compact sizes. Main-body scrolling makes below-fold controls reachable at compact height. These are real ImGui renders, not AI-generated images or live-game screenshots.

Loader preservation was checked using hashes of loader.cpp, ui_theme.h and the D2D ui_renderer.cpp, plus a byte comparison of the executable base before/after payload packaging. All remain unchanged. The packed CiyE.exe contains the new overlay DLL and current runtime. No game was launched for verification.
