# Skin Inventory

Inventory stores the active selection for each character or weapon group. Selecting a skin name or Apply in Characters, Knives or Guns immediately replaces the corresponding slot and saves it. Older inactive history is removed when loading.

Preferences are stored at `%LOCALAPPDATA%/CiyE/BloodStrike/skin_inventory.bin`, with an IPC-directory fallback when that environment variable is unavailable. Writes use a temporary file followed by atomic replacement. An existing empty inventory does not resurrect legacy configuration selections. Invalid files are reported and retained instead of being overwritten with defaults.

Inventory shares the search field and offers Remove. Saved entries remain visible before the catalog loads. Remove stops automatic application of that selection; it does not immediately restore a model already displayed in a match.

## Automatic application

Auto apply saved skins waits for the combat avatar and compatible equipment, then queues requests. Session, character, equipment and weapon-case changes refresh the queue. For overlapping compatible melee categories, the most recent selection takes precedence. Legacy character/gun/knife selections are migrated only when no inventory file exists and the catalog is available.

Gun catalog entries are grouped through `lobby_item_upgrade_data`. Only the highest valid permanent level is displayed for each family. Names are not used as identity keys, and older template mappings remain available for saved selections. Artifact entries with subtype 210 are reference-only because they require blade, handle, core and effect attributes rather than one template ID.

## Validation and limits

The original inventory pass had 40 isolated tests; later catalog coverage increased that historical count to 45. These used mocked game objects, including session transitions, empty/corrupt files, equipment loading, compatible melee classes and re-enabling automatic application. They did not prove rendered behavior in a live match. Later effect integration is documented in [the skin-effects report](../reports/weapon-effects/2026-11-05_skin-effects-fix-report.md).

Skin application requires compatible character/equipment classes and available model resources. `update_runtime.py` is a legacy migration script that can regenerate a runtime from a backup; inspect its inputs before using it with newer source.
