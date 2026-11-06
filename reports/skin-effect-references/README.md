# Skin effect reference findings

This is the earlier captured-table analysis. Its access failures and unresolved roles describe that original pass. Later live resolution of effects and deathbox resources is documented in [the skin-effects report](../weapon-effects/2026-11-05_skin-effects-fix-report.md).

Catalog families: 1167; families with references: 197.
Unique IDs: {'bind_items': 294, 'kill_hud_id': 16, 'hit_effect': 1}.

kill_hud_id is kill-feed/HUD metadata. hit_effect describes a hit reference. Neither proves a kill VFX or a deathbox model.
bind_items IDs are verified links to bundled items; their roles remain unresolved because the corresponding item/effect tables are absent from this capture.
No deathbox effect is marked confirmed. Current live process access was denied; the game was not modified.

| Skin family | Highest captured item | Observed reference IDs |
|---|---:|---|
| M4A1 - Tiadraco | 510010117 | bind_items: 1100013220, 6042000097, 6150000040, 6160000020 |
| M4A1 - Eternal Oracle | 510010207 | bind_items: 6042000098, 6150000050, 6160000021 |
| KAG-6 - Inkforge | 5100140116 | bind_items: 1100142800, 6040010220 |
| AUG - Empyrean | 5100290096 | bind_items: 1100291100, 6140000020 |
| Galil - Fright | 5100380266 | bind_items: 1100385690, 6042000391 |
| P90 - Empyrean | 5200000686 | bind_items: 1200241100, 6040010030 |
| MP5 - Empyrean | 520020096 | bind_items: 1200021100, 6140000090 |
| MP5 - Guard | 520020197 | bind_items: 1200025040, 6042000238, 6150000120, 6160000044 |

## Current saved Inventory

| Saved skin | Observed reference IDs |
|---|---|
| EMMA - BEAR PAJAMAS | bind_items: 6120000330 |
| KAG-6 - Inkforge [Lv 4] | bind_items: 1100142800, 6040010220 |
| MP5 - Dominator [Lv 6] | bind_items: 1200021580, 6140000130 |
| AUG - Riptide [Lv 5] | bind_items: 1100295310, 6042000248, 6150000140, 6160000046 |
| AK47 - Dark Matter [Lv 8] | bind_items: 6170000010 |
| The Spear of Longinus | No references in captured fields |
| RAN - Wintry | No references in captured fields |
| M700 - Bloodlock [Lv 7] | bind_items: 6042000347, 6150000250, 6160000081 |
| P90 - Empyrean [Lv 6] | bind_items: 1200241100, 6040010030 |
| ETHAN - Cyberhunter | bind_items: 6120000120 |
| Galil - Aurelian [Lv 7] | bind_items: 6042000162, 6150000090, 6160000041 |
| Vector - Polarity [Lv 7] | bind_items: 1200086200, 6042000359, 6150000270, 6160000090 |
| Dual Blades - Ultrahard Blade | No references in captured fields |
| M4A1 - Eternal Oracle [Lv 7] | bind_items: 6042000098, 6150000050, 6160000021 |
| Zanpakuto - Tensa Zangetsu | No references in captured fields |

Active Inventory: 15 saved selections; 15 matched to captured families (status: valid).
Full source item/level traceability is in references.csv and references.json. Sources were not written or changed.
