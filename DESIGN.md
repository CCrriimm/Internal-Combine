---
name: CiyE native interface
description: Source-adapted charcoal and lavender Windows menu and loader
colors:
  accent: "#9B8AF4"
  text: "#EDEDEF"
  muted: "#A4A6AE"
  ground: "#0F1012"
  surface: "#16171A"
  border: "#292A2F"
  elevated: "#212226"
  tertiary: "#7D808A"
  success: "#3DCB82"
  danger: "#F0555A"
  warning: "#F2B441"
  on-accent: "#111214"
  primary-label: "rgb(7%, 5.5%, 11%)"
  primary-hover: "rgb(73%, 65%, 100%)"
typography:
  title:
    fontFamily: "Segoe UI Semibold, Microsoft YaHei, SimHei, ImGui default"
    fontSize: "20px"
    fontWeight: 600
  heading:
    fontFamily: "Segoe UI Semibold, Microsoft YaHei, SimHei, ImGui default"
    fontSize: "14px"
    fontWeight: 600
  body:
    fontFamily: "Segoe UI, Microsoft YaHei, SimHei, ImGui default"
    fontSize: "13px"
    fontWeight: 400
  small:
    fontFamily: "Segoe UI, ImGui default"
    fontSize: "11.5px"
    fontWeight: 400
rounded:
  window: "12px"
  child: "10px"
  navigation: "8px"
  control: "6px"
  toggle: "9px"
spacing:
  window-x: "24px"
  window-y: "18px"
  frame-x: "10px"
  frame-y: "6px"
  item-x: "10px"
  inner-x: "8px"
  nav-gap: "2px"
components:
  button-primary:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.primary-label}"
    rounded: "{rounded.control}"
    height: "40px"
  button-secondary:
    backgroundColor: "{colors.elevated}"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "6px 10px"
  input:
    backgroundColor: "{colors.elevated}"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "6px 10px"
  navigation-active:
    backgroundColor: "{colors.elevated}"
    textColor: "{colors.text}"
    rounded: "{rounded.navigation}"
    height: "36px"
    width: "172px"
  toggle-on:
    backgroundColor: "{colors.accent}"
    rounded: "{rounded.toggle}"
    width: "32px"
    height: "18px"
  status-badge:
    backgroundColor: "{colors.elevated}"
    textColor: "{colors.muted}"
    rounded: "{rounded.toggle}"
    height: "18px"
  startup-panel:
    backgroundColor: "{colors.surface}"
    rounded: "{rounded.child}"
    padding: "18px 20px"
---

# Design System: CiyE native interface

## Overview

**Creative North Star: "The CiyE control surface"**

A compact native Windows control surface adapted from the user-supplied 34Cheat UI source. Charcoal surfaces, a lavender accent, a cube mark and restrained line icons connect the menu and loader while retaining CiyE branding and Blood Strike feature content.

This refresh merges the earlier interface constraints with the UI reference implementation in `reference/34Cheat-UI`. The six donor files are retained under `reference/34Cheat-UI`. The donor uses Direct2D and depends on unavailable original project headers; the implemented port uses the existing Win32/DirectX 11 ImGui backend. Shared style and widgets live in `bs-cheat/src/ui_theme.h`; menu and loader composition live in `overlay.cpp` and `loader.cpp`.

**Key Characteristics:**

- Compact charcoal surfaces and lavender state accents.
- A fixed navigation rail with animated selection.
- Flat content groups and independently scrollable catalogs.
- Session status derived from actual application state.

## Colors

The frontmatter records the current shared C++ constants and supersedes the earlier screenshot-derived palette. Accent marks navigation icons, selected tabs, switches, sliders and the primary action. Text and muted serve primary and supporting labels; tertiary is a quieter neutral. Ground separates the rail and loader backdrop from surface content. Elevated defines controls and the selected navigation row; border defines narrow divisions. On-accent supplies dark contrast within lavender marks; primary-label and primary-hover record the loader action overrides. Success, danger and warning communicate real state.

## Typography

Segoe UI supplies body text; Segoe UI Semibold supplies titles and group headings. Overlay font loading includes Microsoft YaHei and SimHei candidates with Chinese glyph coverage, with ImGui's default font available. Loader fonts fall back to its body font. The overlay atlas loads fonts at 16 and 21 pixels, then the shared style sets the base to 13 pixels and page/group headings explicitly render at 20/14 pixels. Loader roles explicitly use 20/14/13/11.5 pixels, scaled by `g_scale`. Consolas is loaded for loader monospace use; it is not a second brand typeface.

## Layout

The overlay defaults to 900 × 620 pixels. Normal minimum dimensions are 860 × 540, reduced by available display bounds; maximum dimensions use display minus 24 pixels, with code floors of 640 × 420. The rail is 196 pixels wide. Navigation starts at (12, 72), with 172 × 36 rows on a 38-pixel pitch. The mark is 22 pixels at (20, 20). Titles start at (220, 20); content begins at (220, 70), uses window width minus 244 and height minus 82, and scrolls independently. Groups resize to their content. Skin category and item panes scroll independently.

The loader's base size is 680 × 520 pixels, scaled through `S()`. A 44-pixel title strip sits above a 264-pixel action column; outer margin is 28 and inter-column gap is 32. The right startup panel starts at x=324 and y=58, with 16 pixels remaining at the right and bottom. Timeline rows advance by 72 pixels. The primary action is 40 pixels high; secondary mode choices are 127 × 32. These are native pixel geometries, not web breakpoints.

## Elevation & Depth

Most separation comes from tonal surfaces and one-pixel borders. The menu uses the donor-style eight-layer ambient shadow: expansion up to 28 pixels, vertical offset 12, and squared falloff capped by the source alpha formula. Content groups remain flat. The loader uses an undecorated rectangular client surface; its ImGui window rounding and border are zero.

Navigation and switches interpolate with `1 - exp(-20 * DeltaTime)`. Preserve that frame-rate-independent motion. Slider tracks are four pixels high with a light circular thumb (radius 5.5, emphasized radius 7).

## Shapes

Shared rounding is defined in frontmatter. Popups use 8 pixels, grabs 5, scrollbars 6 and tabs 4. Scrollbars are 3 pixels wide. Toggle tracks are 32 × 18 within 30-pixel interaction rows, with a 6-pixel-radius thumb. The lavender logo tile has a 0.28 × size corner radius and three dark cube faces, with face opacity inherited from the donor geometry.

## Components

Navigation retains Visuals / Aim / Skins / Settings with authored eye, target, shirt and settings line icons. Selection uses an animated elevated rectangle and lavender icon. The rail has no footer.

Skins retains Characters / Knives / Guns / Inventory, search, rarity labels, current-hero filtering, explicit reload and auto-apply controls. Preserve highest valid gun-level filtering. Artifact skins remain reference-only with disabled apply actions and an explanatory message.

Inventory contains only the active saved choice per hero/weapon group. Clicking a selectable skin name or Apply replaces and atomically persists its slot at `%LOCALAPPDATA%/CiyE/BloodStrike/skin_inventory.bin`; older inactive history is discarded on load. Inventory exposes Remove, includes saved choices in search without a loaded catalog, and reports read/write failures without replacing a damaged file with defaults. Auto apply follows the existing global switch and matches loaded combat equipment, including training and subsequent matches; the latest compatible knife selection wins when melee categories overlap. These choices do not establish account ownership.

Loader retains its existing modes, start options, callbacks, connected startup timeline and collapsed activity details. Status markers reflect real process/session state; do not invent percentages or readiness.

Keyboard navigation is enabled in both ImGui contexts. Tab/Shift+Tab move focus; native button activation supports keyboard use for navigation and switches. Sliders retain native keyboard adjustment and Ctrl+click numeric entry with clamping. Insert is the default configurable menu toggle; Escape closes the open menu. The close control also hides the menu. These controls remain available without adding the excluded sidebar hint.

## Do's and Don'ts

- Do retain CiyE naming and the existing feature content.
- Do use native ImGui interaction for keyboard focus, activation and clamped slider values.
- Do keep catalog and Inventory errors visible and content reachable through scrolling.
- Don't add a sidebar footer, attachment dot, attachment text or Insert hint.
- Don't copy Roblox-only product content or fabricate session readiness.
- Don't describe Inventory choices as proof of account ownership.
- Don't treat historical screenshots or syntax checks as rendered validation of this replacement.

Source inspection and MSVC `/Zs` syntax checks of overlay and loader are the validation available for this pass. No executable was built or launched, following the user's constraint. No rendered visual validation was performed. Earlier screenshots are historical. Sidecar HTML/CSS snippets are standalone design-panel translations of native widgets, not screenshots or runtime evidence.

## In-game menu: ImGui / Direct3D 11

The in-game menu uses ImGui exclusively. Its overlay target does not compile Menu34/D2D sources or link d2d1/dwrite; the loader keeps its existing renderer, source and style. Menu styling is isolated in `src/menu_theme.h`, while the loader continues to use `src/ui_theme.h` and its D2D renderer.

Menu-only tokens: accent #35C6D9 (saved theme may override it), text #E9EFF7, secondary #99A9BC, rail #0D1117, window #10151D, panels approximately #151B25, controls #1C2430 and borders #27313E. Body text is 14px, section headings 14px semibold and page titles 22px. Default window is 960 × 680, minimum normally 820 × 560; rail width is 188px with 42px rows and a 48px step. Main content scrolls independently per tab. Panel groups use content-driven height with 18px horizontal padding, and columns have a 24px gap. No sidebar footer is added.

Visuals groups elements, filters, style and collapsible colors. Aim groups targeting, activation, tracking/humanization and recoil. Settings preserves saving, accent themes, Reduced Motion and session controls. Skin browsing and Inventory continue through the established ImGui flow. Switches and sliders keep native keyboard input; human smoothing is visibly disabled when Humanize is off.

Real render captures for four tabs at default and compact sizes live in `.impeccable/review/imgui-menu/`. The isolated `bs_menu_preview` target includes the actual menu code and renders through D3D11 WARP without game hooks or user config loading; skin data in those images is a fixture. Loader source/style/renderer hashes and the loader executable base were verified unchanged when packaging. Game-session visual behavior remains unverified.
