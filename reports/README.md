# Logic and function research

| Area | Findings and evidence |
|---|---|
| Skin effects and bullets | `weapon-effects/2026-11-05_skin-effects-fix-report.md`; `weapon-effects/weapon-effects.json`; `weapon-effects/all-menu-guns.csv`; function metadata/disassembly and effect-gate observations |
| Recoil | `recoil-probe/2026-11-05_recoil-report.md`; function signatures, disassembly and observed call counters |
| Runtime performance | `performance/2026-11-05_runtime-lag-report.md`; before/after helper timings |
| Custom-room entities | `custom-room/2026-11-05_custom-room-report.md`; player/team/HP/position snapshots and field mapping |
| Offsets | `offsets/offset_report.txt`; `offsets/offsets.json`; SDK and header under `../reference/offsets` |
| Combat logic | `combat-logic/bloodstrike_combat_functions.md`; captured function metadata and bytecode |
| Skin-family references | `skin-effect-references/README.md`; family and active-selection reference tables |
| Shot-direction functions | `aim-mode-probe/contract.json`; `gun_spell.dis.txt`; `gun-callers.dis.txt` |

Tools are under `../tools`. `offset_dump.py` and `export_skin_effect_refs.py` use repository-local SDK/catalog inputs by default. Each report states its evidence and validation limits; table presence alone does not prove that an effect was rendered for every skin or mode.
