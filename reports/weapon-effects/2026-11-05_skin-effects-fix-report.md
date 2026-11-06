# Skin effect binding and bullet visuals

Recorded on 2026-11-05. The integration connects a selected appearance to its client-side visual effects while retaining original network/entity data.

## Changes

Appearance application refreshes the item ID, extra item metadata, effect cache, first-person properties and racer component. An unchanged model template can still require metadata/effect repair. A ready-state check avoids repeated reloads; failures remain retryable.

The original show_guise_bullet_trace/show_guise_hit_effect cached getters read the server weapon's display_guise_item_id, which could still describe the original appearance. Scoped getters use the selected item/template on marked local cases in the same space. Other cases retain the original getter. Effects are enabled only when the corresponding feature and template resources exist.

The game retains responsibility for FPS/ADS/TPS tracers, muzzle effects and material-specific impacts. The integration does not manufacture effect IDs for skins without those resources. Kill-feed rendering receives a copied packet for the local caster and matching weapon. Scores, remote players and the original packet remain unchanged.

AvatarModel.PlaySkinDeadAnimAndEffect uses an isolated decode context for one render call to resolve kill_fx, dissolve materials and existing team-kill variants. It does not replace replicated final_kill_info or global pickle.loads. Local KillAvatar/KnockDownAvatar callbacks receive the selected item identity for the game's sound/screen-effect checks. Original callback results are preserved.

Bindings are cleared across space/owner changes. Loot-crate cosmetics are separate from team-kill extra models, and use_dynamic_box controls model bounds rather than deathbox selection.

## Evidence and validation

E-before: current-effect-state-main-thread.json records M4A1 guise 11100018/item 5100000950 with tracer/hit flags False. E-after: installed-effects-2.json records the same appearance with both flags True, 10 hit-material configurations, new_fire_sfx_id=818 and new_fire_ads_sfx_id=819. Application and state reads ran on an engine callback rather than invoking engine APIs from a Frida worker thread.

Getter evidence is in effect-gates-targeted.json. Function evidence is in function-metadata.json, apply-and-bullet-functions.json, skin-refresh-functions.json and the readable disassembly files.

- Bullet path: appearance binding -> clear item cache / initialize FPS properties -> scoped case getters -> gshare.effect_util.PlayBallisticEffect.
- Feed path: caster/weapon packet -> local appearance metadata copy -> PushKillRecord.
- Kill-render path: final_kill_info -> isolated decoder -> PlaySkinDeadAnimAndEffect -> PlaySkinDeadEffect / PlayKillTeamAnim.

The author confirmed sufficient effects before the final build request. This does not establish that every catalog skin or mode was rendered during validation. The final checkpoint passed 154 isolated tests covering identity repair, tracer/hit availability, skins without tracers, local caster/weapon/space filtering, packet preservation, team-kill handling, isolated decoding, callback behavior and retry after resource-loading failure.

## Catalog exports

weapon-effects.json and all-menu-guns.csv describe 743 menu entries (741 unique gun templates). The capture contained 119 entries with kill_fx, 125 with a replacement feed icon, and 23 with a family-bound deathbox cosmetic. Resource lookup resolved 195 effect records and 85 model records. Table presence does not establish downloaded DLC, account entitlement or activation in a match.

active-inventory-guns.csv retains source-item candidates for aliased saved templates. inventory_skin describes the saved choice; skin/menu_upgrade_level describe the corresponding menu family entry. API identifiers, game strings and resource paths remain verbatim evidence. Host paths in distributed captures are normalized.
