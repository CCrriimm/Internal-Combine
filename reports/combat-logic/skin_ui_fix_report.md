# Skin-flow and interface changes

This historical pass updated the appearance-selection flow and its UI.

## Interface

The menu and loader shared a charcoal palette, purple accent and Segoe UI styling. Skin subtitles and loader checkboxes were aligned explicitly after line breaks. Toolbars wrap when space is limited, lists scroll independently, and gun/melee catalogs use separate groups. Reload catalog and catalog-file change detection were added.

## Appearance flow

Model skins use ChangeWeaponGuise with a template ID and its associated item ID. Catalog generation preserves permanent high-tier items and upgrade levels. Character selections must match the current hero; melee selections must match the equipment class. Artifact attributes cannot be replaced with a single ID. Status messages distinguish an update request from independently verified rendered success.

## Historical validation

The pass recorded 20 isolated skin tests for each runtime copy, MSVC syntax checks for overlay and loader, and fixture-only UI previews. Runtime AST comparison limited the changes to skin functions. The actual loader, packaged executable, game rendering and kernel mode were not exercised in that pass.

A configuration-version mismatch was observed without overwriting saved preferences. The legacy update_runtime.py script may overwrite newer changes when manually regenerating from a backup. Later inventory/effect reports describe subsequent revisions; historical UI screenshots are not evidence for a newer renderer.
