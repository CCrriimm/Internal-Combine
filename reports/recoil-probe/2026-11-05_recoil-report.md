# BloodStrike recoil functions observed through Frida

Captured on 2026-11-05 in the shooting range. A signature-checked embedded-Python bridge examined 2,610 loaded gclient/gshare modules and selected 93 related code objects. A 20-second sys.monitoring.PY_START observation recorded calls without replacing function bodies, arguments or results. Local events and the monitoring tool ID were removed before detach.

## Main recoil state machine

Functions are in game module gclient.framework.camera.fps_state_fsm. Line numbers come from the captured code objects.

| Function | Runtime line | Role | Observed calls |
|---|---:|---|---:|
| StateRecoil.OnRecoilEnter(self, shoot_idx) | 535 | Resolve pattern/random values, initialize yaw/pitch and start the up phase | 62 |
| StateRecoil.RecoilUp(self) | 587 | RotateEasing with yaw/pitch magnitudes and up duration | 62 |
| StateRecoil.RecoilFall(self) | 598 | Fall phase using RecoilFallPct | 61 |
| StateRecoil.RecoilStatic(self) | 615 | Optional pause before the down phase | 61 |
| StateRecoil.RecoilDown(self) | 627 | Recovery, including cycle and persistent-recovery branches | 61 |
| StateRecoil.OnRecoilExit(self) | 644 | Exit recoil state | 61 |

Callbacks link up -> fall -> static -> down; the down callback returns the FSM to Static. Counts may differ because observation begins/ends within a cycle. They are not an exact bullet count.

## Weapon pattern and random data

Functions are in gclient.gameplay.logic_base.equips.weapon_case.

| Function | Runtime line | Role | Observed calls |
|---|---:|---|---:|
| WeaponCase._UseSpecRecoilData | 3607 | Select special-pattern data | 2 |
| WeaponCase._GetRecoilDataByShootIdx | 3624 | Index a pattern and its cycle | 2 |
| WeaponCase.GetRealRecoilData(self, is_real_ads, shoot_idx) | 3635 | Resolve actual pattern with ADS/spec/talent adjustments | 62 |
| WeaponCase.GetRealRecoilRandomData(self, is_real_ads, shoot_idx) | 3655 | Resolve random yaw/pitch bounds | 62 |
| WeaponCase.GetRealPersistRecoilDownTimeMax | 3673 | Limit persistent recovery duration | 0 |

_GetShootAttr/_UpdateShootAttr cache resolved data. The original pattern-read count therefore need not equal GetRealRecoilData calls. PlayerCombatAvatarMember.DealRecoilDataByTalent in cimp_talent, line 22, was also called 62 times.

## Recovery and camera roll

StateRecoilRecover has enter/callback/exit functions at lines 679/696/710 and EnablePersistRecoilRecover at line 700. Only EnablePersistRecoilRecover was observed here, with 4 calls.

StateRollShake.OnRollShakeEnter was called 62 times, OnRollShakeTick 1,173 times, and OnRollShakeExit 61 times. WeaponCase.GetRealRollShakeInfo, line 3794, was called 62 times. This is a separate camera-roll path alongside recoil.

AssistAimAndShootTracing.RecoilStart/RecoilEnd and NoSearching variants were present in metadata; presence alone does not establish execution.

## Snapshot and evidence

The captured FpsPlacer was Static with shoot_idx=20, RecoilPitchRange about 0.014, RecoilYawRange about -0.00143463, RecoilUpTime=0.04, RecoilStaticTime=0, RecoilDownTime=0.15 and forcePersistRecoilRecover=False. These are current-state values, not universal weapon constants.

E-imports: weapon-metadata.json contains the first selected module inventory. E-001: recoil-metadata.json contains signatures, aliases and disassembly. E-002: recoil-metadata.trace.json contains counters, the first 256 events and monitoring_removed=true. E-003: recoil-functions.dis.txt is the readable disassembly; recoil-functions.json is the focused catalog. sha256.json covers the named distributed artifacts.

The main call path is pattern/random resolution -> OnRecoilEnter -> RecoilUp -> callback -> RecoilFall -> callback -> RecoilStatic -> callback -> RecoilDown, with cameraEasing.RotateEasing applying rotation. This path is supported by bytecode and observed calls. Persistent-recovery activation conditions remain unproven in this trace.

Counts are interpreter-wide rather than per instance. Shared decorator wrappers cannot be attributed to a single callback; original callback closures were captured separately. Only the first 256 events are retained, while counters cover the entire window. Existing unrelated runtime hooks were not changed. No native RotateEasing RVA was established.

```powershell
$gamePid = (Get-Process BloodStrike | Select-Object -First 1).Id
python tools/probe_recoil_frida.py --pid $gamePid --output reports/local/recoil.json --trace-seconds 20
```

Omit --trace-seconds for metadata only. The probe detaches and removes its monitoring state on normal cleanup.
