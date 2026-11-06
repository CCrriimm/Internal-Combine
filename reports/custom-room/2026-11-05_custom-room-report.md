# BloodStrike custom-room player fields

Captured on 2026-11-05 through a signature-checked embedded-Python bridge. Two snapshots contained 3 combat avatars among 11 entities, then 4 among 16. This established Python property access, not new native HP/team offsets.

| Data | Access path | Observation |
|---|---|---|
| Combat entities | genv.space.combat_avatars | Dictionary with 3, then 4 entries |
| All entities | genv.space.entities | Includes non-player objects |
| Current player | genv.player | Changed from PlayerCombatAvatarSquadFight to ReplayCombatAvatarSquadFight |
| Identity | entity.id | String identifier |
| Team | entity.combat_team_id | team_id was absent on the captured avatars |
| HP | entity.hp / entity.server_hp | Values matched |
| Current maximum HP | entity.cur_maxhp | One value was 225 while maxhp was 125 |
| Position | entity.position / entity.pos | Matching finite 3D coordinates |
| Armor | entity.armor / entity.server_armor | Captured values included 0 and 50 |

Remote CombatAvatarSquadFight instances had IsCombatAvatar=True but IsPlayerCombatAvatar=False, IsRobotCombatAvatar=False and IsDummyCombatAvatar=False. The original concrete-player/bot filter therefore rejected them. The maximum-HP fallback also missed cur_maxhp.

Snapshot 2 contained HP=141 with cur_maxhp=125. Sequential client reads are not an atomic server-state transaction, so this observation is retained instead of enforcing HP <= maximum. Replay player identity must not be assumed to identify the original local avatar.

## Existing SDK fields

The SDK timestamp matched the executable examined during capture. Offsets below are decimal and relative to their respective native objects; they were not independently verified against native objects during this pass.

| Class | Member | Offset |
|---|---|---:|
| IGameplay | Player | 88 |
| ClientPlayer | Camera | 3464 |
| ClientPlayer | PrincipleRole | 3544 |
| IEntity | Transform | 88 |
| IEntity | Level | 144 |
| IEntity | World | 152 |

Do not add these values to Python id(entity). Transform is Matrix4x3; no translation-member layout was inferred. No native HP/team offset was established.

## Evidence and reproduction

E-001 and E-002: snapshot-20261105-1.json and snapshot-20261105-2.json. E-imports: imports.json, an import inventory of the examined PE. fields.json records the selected SDK members and Python access paths. sha256.json covers the named published artifacts.

The captured Space.enter code registers IsCombatAvatar entities in combat_avatars. CombatAvatarMember declares combat_team_id/server_hp and exposes hp; the game's health bar uses owner.cur_maxhp. Together these support the path genv.space -> combat_avatars -> team/HP/position properties.

```powershell
$gamePid = (Get-Process BloodStrike | Select-Object -First 1).Id
python tools/probe_custom_room.py --pid $gamePid --output reports/local/custom-room.json
```

The probe reads client-replicated entities and detaches after capture. It does not establish complete room/map visibility. Published evidence paths are normalized for distribution; API identifiers and game values are retained.
