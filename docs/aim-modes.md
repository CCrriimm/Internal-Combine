# Aim modes and no recoil

Implemented 2026-11-05. The Aim page selects exactly one mode: Aimbot or Silent Aim. The shared enable switch, activation key/toggle, target filters, bone priority, FOV and prediction remain common. The mode uses options bit 5; bit 4 remains the FOV-color marker. The configuration remains 160 bytes and defaults to Aimbot. A compile-time assertion prevents overlap with the existing option bits.

## Silent Aim

Frida attached to BloodStrike PID 17280 and captured metadata/marshalled bytecode from 261 loaded modules. The successful capture contains 1,667 methods in `reports/aim-mode-probe/contract.json`. The probe validates the embedded Python ABI prologues before calling its GIL/run helpers and executes metadata collection in a separate globals dictionary. It does not reload the game runtime.

The exact hook is `gclient.gameplay.logic_base.spell.spell_core.gun_spell.SpellWorker.GetShootDir(self, shoot_pos)`. Its observed bytecode returns a camera ray with native scatter applied. `SpellWorker.SpellStrike.func` first restricts the caster to `genv.player`, then uses GetShootDir's return as `shoot_dir_fps` and passes it into the shot geometry/result flow. Disassembly is saved in `reports/aim-mode-probe/gun_spell.dis.txt` and `gun-callers.dis.txt`.

The runtime hook calls the original direction function once to preserve native bookkeeping, then substitutes only the local player's shot direction when Silent Aim is active. It uses the latest selected target and current shot origin. It rejects a closed activation gate, menu opening, hall state, a dead/stale target, changed space, invalid range/FOV, or an unavailable interface. Those cases retain the native result. Aimbot selection/toggle-off/hall state restores the original class method without overwriting a later external replacement.

Silent Aim bypasses camera turning, humanization and tracking smoothing. Prediction excludes camera smoothing lag in this mode. The UI omits the smooth and humanization controls while retaining their values for Aimbot. This implementation targets the verified `gun_spell` path; launcher/bow/other independent spell implementations have not been adapted.

## No recoil

The old fixed downward camera pull was removed. The shared No recoil switch now wraps the observed FpsPlacer RecoilUp/RecoilFall/RecoilDown methods. Their recoil magnitudes, cycle recovery percentage and persistent recovery flag are neutralized only during each native call, then restored in a finally block. The native FSM callbacks still run. Hooks are removed when disabled, leaving the match or replacing the camera placer. It works independently of the aim activation gate and applies in either mode. Native spread and unrelated camera effects remain separate from recoil.

## Validation and limits

The C++ overlay/menu-preview targets build. All 138 tests pass, including mode exclusivity, no camera/humanization calls from Silent Aim, no smoothing-lag dependency, original shot bookkeeping, local-caster restriction, stale/invalid-target fallback, hook restoration, recoil suppression during aiming, exception cleanup and option persistence. Both menu modes were rendered and visually inspected.

A separate eight-second Frida passthrough trace installed GetShootDir interception and restored the original successfully. It observed **zero shots** during that interval. This is evidence of installation/cleanup only, not successful redirection, hit registration or live no-recoil behavior. No active Silent Aim/no-recoil patch was injected into the running session. The packaged feature build needs a fresh game session for live validation.
