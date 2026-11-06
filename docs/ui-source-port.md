# UI source port

The UI reference implementation is preserved under `reference/34Cheat-UI`: `loader.cpp`, `loader.hpp`, `menu.cpp`, `menu.hpp`, `ui_renderer.cpp` and `ui_renderer.hpp`.

The donor uses Direct2D and unavailable original project headers. The implemented adaptation retains the existing native Win32/DirectX 11 ImGui backend and shares palette, logo, navigation, switches, sliders and shadow helpers in `src/ui_theme.h`. Donor geometry and visual vocabulary inform the menu and loader; existing feature content and callbacks remain. CiyE branding and the explicit absence of a sidebar footer are durable constraints. The current tokens and composition are documented in `DESIGN.md`; `.impeccable/design.json` supplies non-normative standalone design-panel previews.

Preserve active-only saved Inventory selections, highest valid gun-level filtering and reference-only Artifact rows. Inventory presents user selections, not account ownership. This presentation work does not authorize changes to runtime, injection or driver behavior.

Validation reported for this pass: MSVC `/Zs` syntax checks passed for overlay and loader. No executable was built or launched, as requested. No rendered visual validation was performed; previous screenshots do not validate this source replacement. The documentation pass inspected source and validated sidecar JSON only.
