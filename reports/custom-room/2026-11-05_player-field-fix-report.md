# Player classification and maximum-HP fixes

Recorded on 2026-11-05. The shared filter now accepts remote objects marked IsCombatAvatar whose class ancestry starts with CombatAvatar, while rejecting vehicles and skill objects first. HP, team or generic combat flags alone remain insufficient.

Maximum HP prefers cur_maxhp before existing fallbacks. Local identity uses genv.player itself when it is a combat avatar with no explicit controlled-avatar link, before falling back to a potentially stale genv.avatar.

## Evidence and validation

| Check | Result | Evidence |
|---|---|---|
| Python regressions at this checkpoint | 143 passed | Isolated unittest suite |
| Recorded custom-room snapshots | 7 avatar observations accepted; current maximum HP selected | fix-snapshot-regression.json |
| Live helper replacement | Three helpers applied without restarting the runtime | fix-lobby-1.json |
| Live shooting range | Local avatar plus 6 bots recognized | fix-live-2.json |
| Live custom/ranked modes after this patch | Not independently established by this capture | Manual validation remained separate |

The filename fix-lobby-1.json was chosen before capture; its content is a shooting-range session, not a lobby observation. The custom-room regression reused recorded data rather than a new live custom-room session.

Evidence path: genv.space.combat_avatars -> shared classification -> entity query; entity.cur_maxhp -> frame serialization; genv.player -> local identity -> self/team filtering. Gameplay configuration and the IPC format were unchanged by these fixes.

```powershell
python -m unittest discover -s tests
$gamePid = (Get-Process BloodStrike | Select-Object -First 1).Id
python tools/probe_custom_room.py --pid $gamePid --output reports/local/player-fields.json --runtime-source pyexec/bs_runtime.py
```

The helper-evaluation option refers to historical helper names and may require adapting to later naming changes. Published capture paths are normalized; native layout validation and rendered behavior in every mode are outside this checkpoint's evidence.
