# ESP and aim filters

ESP always excludes vehicles, including objects that expose HP or a combat team. Eligibility requires explicit combat-avatar or supported monster flags. Player avatars riding vehicles remain eligible. Friendly bots use the same team checks as players. Team IDs are normalized across integer/string representations; empty IDs fall back to valid team_id or the local player. Same-team identity and IsTeammate take precedence over CanDamage so friendly fire does not turn allies into enemies. Without a known player relationship, teammates-off hides that player until metadata arrives.

Visuals exposes Enable ESP and Show teammates at the top, and scrolls to the remaining settings. Show teammates uses the existing saved F_ESP_TEAM bit. ESP counter rendering follows that setting as well. There is no vehicle toggle, per the user's choice to always exclude them.

Aim / Targeting exposes Ignore knocked targets. The option uses bit 1 of the existing reserved options field, preserving the serialized config layout and hold/toggle bit 0. It defaults off for existing/new configurations. When on, both fresh and retained selection skip knocked targets; current combat state also catches a knock after the last entity gathering. It changes aim selection only, leaving knocked ESP display available.

Validation: 22 isolated ESP/aim regression cases and 45 existing skin cases passed. MSVC syntax-only checks cover overlay.cpp and ui34/menu34.cpp. No application executable was built or launched; live-game classification and rendered layout remain unverified. Existing unrelated source/working-tree changes were preserved.

## Prediction and Magic Field update

F_AIM_PREDICT now changes the final aim direction after both fresh and retained selection. Each combat entity has a bounded motion history with monotonic timestamps; velocity is measured from world-position differences and smoothed over 40 ms. Data older than 250 ms, space changes, invalid coordinates and corrections above 100 world units/s are rejected/reset. Tracking lead uses the same smoothing fraction as the camera, with compensation bounded to 250 ms plus sample age. Prediction off returns the original point; sampling stays warm.

If the current weapon exposes a finite positive bullet_speed, projectile_speed or muzzle_velocity on its case/proto, a constant-velocity intercept is solved and up to one second of flight lead is added. The total prediction horizon is capped at 1.25 s. These candidate property names and speed units have not been verified in a live game. When unavailable, prediction compensates tracking delay only; it does not invent projectile speed. Gravity and network ping are not modeled.

Magic Field exclusion precedes all combat-avatar/monster acceptance. Both the IsMagicField property/method and MagicField class ancestry are checked. Destructible fields with HP/team/combat flags are rejected; ordinary players with a magic_field_id remain eligible. This shared query filter affects ESP, its counters and aim candidates.

Latest validation: 26 isolated ESP/knock tests, 13 prediction tests (including simulated reduction of steady running-target tracking lag), and the 45 existing skin tests pass. The live runtime is not imported or started by these tests. No new EXE was built and live-game lead accuracy remains unverified.

## Best target and strict skill exclusion

Candidate acquisition ranks allowed enemies ahead of allowed teammates, then standing targets ahead of knocked targets. Within that priority group, lower score wins: 0.85 × angular offset / half-FOV + 0.15 × distance / max range. A recent current target gets a 0.06 score preference, so near-equal candidates do not alternate every frame; clearly better candidates still win. Equal scores use range and entity ID to resolve ties deterministically. Target release or an empty candidate set clears retention.

Retained targeting calls the same selector, including actual selected-bone FOV and visibility checks. It cannot bypass those checks to continue through a wall or outside the configured FOV. Prediction is applied after selection and existing team/knock settings remain authoritative.

The shared ESP/aim query accepts explicit player, robot combat avatar, zombie robot, training dummy or ATM guard identities, plus entities marked IsCombatAvatar whose class ancestry starts with CombatAvatar. Remote CombatAvatarSquadFight entities lack the local-player marker. IsCombatAvatar/IsMonster/HP/team alone are insufficient. Skill entity markers and class ancestry exclude Magic Field, attack drones, holographic robots/decoys, projectiles, traps, turrets and summons before any avatar flags are accepted. Player skill IDs, buffs, shields and magic_field_id are not exclusion criteria. This prevents ordinary skill objects with generic combat fields from becoming player ESP while preserving affected players.

HP maximum now prefers cur_maxhp over template maxhp. Local identity uses genv.player directly when it is a combat avatar and has no explicit controlled-avatar link, before falling back to genv.avatar. This prevents a stale avatar from supplying the wrong self ID/team after a mode transition. Live verification and remaining mode coverage are recorded under reports/custom-room/2026-11-05_player-field-fix-report.md.

Latest checks: 41 ESP/target/knock cases, 13 prediction cases and 45 skin cases pass (99 total). Selection tests cover retention versus materially better targets, stable ties, team/knock priority, range/death filters and retained FOV/visibility. Tests use mocked game entities; no executable was built or game session launched.
