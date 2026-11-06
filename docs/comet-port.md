# Comet port: added features

Everything below was ported from the supplied Comet source (the Drive archive,
`Mini-Project-<id>-mini-project`) and adapted to this repository's in-process
layout. Nothing that already worked was renamed, reordered or removed: the
config grew only at the tail, the entity record grew only at the tail, and the
build still produces the same files.

| Feature | Where | Config |
|---|---|---|
| Enemy hero name | Visuals → ESP extras, above the box | `OPT2_ESP_HERO` |
| OBS capture protection | Settings → Protection | `OPT2_OBS_PROTECT` |
| Capture-protection bypass (re-apply) | `src/capture_guard.hpp` | automatic |
| Loot ESP | Visuals → ESP extras | `OPT2_ESP_LOOT`, `loot_max_dist` |
| Max-armor tier colours | Visuals → ESP extras | `OPT2_ESP_ARMOR_MAX`, `col_armor_tier[4]` |
| Team label | Visuals → ESP extras | `OPT2_TEAM_LABEL` |
| Team box/line colours | Visuals → ESP extras | `OPT2_TEAM_COLORS`, `col_team_box[4]`, `col_team_line[4]` |
| Input noise (aimbot only) | Aim → Input | `OPT2_AIM_NOISE`, `noise_strength` |
| SendInput-only mouse output | `pyexec/bs_runtime.py` (`_send_mouse`) | always |

`Cfg` is now 224 bytes (`VERSION` = 8). Every field before `col_team_box` keeps
its original offset — `offsetof(Cfg, col_team_box) == 148` is pinned by a
`static_assert` — which is what makes the v7 -> v8 migration exact: the first
148 bytes of an old file and the old trailing `pad[3]` are copied into the new
struct, the tail is filled with the same defaults a fresh install gets, and the
config is rewritten in the new layout on the next autosave. Nothing is lost and
nothing is guessed. `Ent` is 288 bytes: `hero_id` and `is_dying` are appended
after the 16 bone pairs, so all earlier offsets are unchanged.

---

## 1. Enemy hero name

The runtime already resolved `hero_id` for the local player (skin selection).
The entity record now also carries the replicated `hero_id` of every ESP
subject, and the ESP draws it as a chip above the name.

* Known ids use the existing `HERO_NAMES` / `HeroNameById` table.
* An unknown but present id prints `Hero <id>`; a missing id prints nothing, so
  a target without hero data never gains a fake label.
* Teammates are excluded (the switch is an *enemy* hero name).
* The chip sits above the name chip and below the knock label, so existing
  label order is preserved.

Reference: `src/render.cpp::HeroName` and `cfg::esp_show_hero`.

## 2. OBS capture protection

`src/capture_guard.hpp` (ported from `deps/window/capture_blackout.hpp`) calls

```cpp
SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
```

on the game window, and restores `WDA_NONE` when the switch is turned off, on
"Safe exit", and on "Unload overlay".

It is **on by default** (`DefaultCfg` sets `OPT2_OBS_PROTECT`). Previously
`InitOverlay` unconditionally called `SetWindowDisplayAffinity(g_hwnd, WDA_NONE)`
on every swap-chain bind, which would have undeflaked a user's protection on
each resolution change; that call is gone and the guard owns affinity now.

The Settings row reports the real API result: `active`, `active (re-applied)`,
`SetWindowDisplayAffinity failed` (with the Win32 error) or `game window
unavailable`. It never claims success it does not have.

**Limits.** This is Windows display affinity, not a spoofer. Hardware capture
(e.g. a capture card) and recorders that ignore display affinity still record
the screen, and exclusive fullscreen is a known weak spot. The `F8` visual-hide
path remains the manual fallback.

## 3. The bypass

The reference build's own audit reduced its `bypass.cpp` to a no-op: the current
QSec code paths it targets are not the ones the old IAT/`ReadMem` hooks
assumed. Porting inert hook code would add risk without adding coverage, so the
bypass here is the part that measurably works against the feature's own failure
mode: **the game can clear or reject display affinity at any time**, and the
guard puts it back.

* While protection is requested, `GetWindowDisplayAffinity` is polled twice a
  second; if it no longer reads `WDA_EXCLUDEFROMCAPTURE` the guard re-applies it
  and increments `reapplies` (shown as "active (re-applied)").
* A failed `SetWindowDisplayAffinity` is retried on a 2 s backoff, not every
  frame, and keeps reporting the failure.
* A recreated game window (mode switch, resolution change) is re-protected
  instead of trusting stale state.
* `Restore` in the panic/shutdown path always attempts `WDA_NONE` once,
  ignoring the timers.

`tests/test_capture_guard.py` compiles the header against stubbed Win32 entry
points and drives apply → verify → re-apply → failure → backoff → recovery →
window replacement → restore → lost-window → panic, deterministically.

If a real anti-cheat bypass is wanted later, it needs exact signatures, a
runtime test plan and its own report; guessing at one is explicitly out of scope
here, as it was in the reference.

## 4. Loot ESP

The runtime walks the same entity cache the player ESP uses and keeps an entity
only when its owner type is exactly `CombatItemEntity` or `Supply` — the two
types established by the reference's captured graph. Name similarity is not
used, and no item name or rarity is invented.

* A `Supply` whose replicated `is_open` reads true is hidden. An unreadable
  (`unknown`) flag keeps the container, matching the reference.
* Distance is camera → item, filtered by `loot_max_dist` (10–500 m, default
  120 m) and by the same behind-camera rejection the player ESP uses.
* Position is the item's own transform, projected with the shared world→screen
  function. Items that do not project are dropped.
* The packet carries a trailer after the player array:
  `[u32 count][count × (u64 id, f32 x, f32 y, f32 distance, u32 kind)]`,
  `kind` = 0 dropped item, 1 supply. The header is always present (count 0 when
  the feature is off), and the overlay bounds-checks the count against the
  buffer it actually received, so a short or stale packet cannot produce a
  phantom marker.
* Loot is drawn independently of the player ESP switch: `DrawESP` is gated on
  `F_ESP`, `DrawLootESP` is not.
* The scan is throttled to 4 Hz and cached; it never dereferences unknown
  sub-objects (`sub_items`, `data`) and never writes to the game.

**Unverified:** the reference's loot detection came from a captured graph on its
build. `position` resolution on this build has not been validated in a live
game (no game is launched by the test suite). If the chain does not resolve,
the feature shows nothing rather than guessing — check `ipc/runtime_err.txt`
and the Debug capture if you expect markers and see none.

## 5. Max-armor base colour

`src/esp_armor_tier.hpp` ports `esp_colors::ArmorTier` from the reference:
50 grey, 75 blue, 100 purple, 125 gold, with linear blends for intermediate
replicated maxima (so 87 armour reads blue-with-a-touch-of-purple instead of
snapping to the wrong tier). NaN and ≤ 50 read grey; > 125 stays gold.

The armor **bar** keeps the user's `col_armor`. The tier colour paints the
max-armor value printed beside the bar, which is what makes the tier readable at
a glance. All four tier colours are editable under "Armor tiers".

## 6. Team label and team colours

* **Label** — `teamtint::Labels` assigns `Team 1`, `Team 2`, … in first-seen
  order and keeps a team's number for the match. It is reset whenever the
  overlay sees `in_match` go false, and also after 120 consecutive empty frames
  (~2 s) in case a match ends without that transition, so a new game always
  starts at Team 1. Raw ids such as 2001 are too long for a chip, which is why
  the compact number exists.
* **Colours** — `col_team_box[4]` / `col_team_line[4]` are picked by
  `team id % 4`, so a team keeps its colour across matches.
* Priority is unchanged: knocked (orange), teammate, monster and the
  hidden-by-visibility colour all win over a team tint; the tint only presents a
  living, visible enemy with a known team. Team colours are presentation only and
  never participate in team or aim filtering.
* The label also respects `Show teammates` the same way the rest of the ESP
  does, because the entity list is already filtered upstream.

Reference: `cfg::esp_show_faction`, `cfg::esp_faction_colors`,
`render.cpp::FactionColour` / `CompactTeamNumber`.

## 7. Input noise, aimbot only

Before this port the runtime applied a fixed ±0.0016 rad sine jitter to *every*
view-tracking call. Now:

* `_micro_jitter(dt, amplitude=0.0016)` takes the amplitude; `0.0` (or
  negative, or NaN) returns `(0, 0)` immediately and consumes no time step.
* `_apply_view_transform(..., noise=0.0)` passes the configured amplitude, and
  the cam-tracking call site is the **only** one that supplies a non-zero value,
  and only when `OPT2_AIM_NOISE` is on.
* The tracker and the silent-aim/ray-redirect path therefore keep exactly the
  behaviour they had before the port — the default argument is still the
  historical `0.0016`, so nothing silently changed amplitude.

Noise is applied to the placer's per-frame turn vector (which is what actually
moves the view), is smoothly continuous (two detuned sines, not per-frame
random), and `noise_strength` is clamped to 0–0.008. A test asserts there is
exactly one call site that can pass a non-zero amplitude.

## 8. Mouse input method: SendInput only

`mouse_event` is gone from the runtime, including the fallback that used to run
after it. Every mouse event the runtime injects now goes through one helper:

```python
MOUSEEVENTF_LEFTDOWN / MOUSEEVENTF_LEFTUP -> _send_mouse() -> SendInput(...)
```

Why one path matters: previously a click used `mouse_event` and only fell back
to `SendInput` if that raised, so the two engines could disagree about what the
game actually received. Now there is exactly one `SendInput` call to audit and
no legacy API in the codebase at all — a test fails if `mouse_event` reappears.

Aim movement itself is not synthesised mouse input: the runtime steers through
the game's own view placer (`placer.TurnView`), which is what the aimbot has
always used. `SendInput` is therefore the click path plus the API the aim code
falls back to; nothing else in the runtime touches the cursor.

The overlay's own menu/RCS worker has its own native `SendInput` calls in
`src/overlay.cpp` and was not touched.

---

## Tests

```bash
python -m unittest discover -s tests
```

New/extended coverage (all isolated from the game):

* `tests/test_config_store.py` — the native save/load/migration harness, now
  asserting the v7 -> v8 migration keeps user settings, fills the new tail with
  defaults, leaves the config dirty for rewrite, and still *rejects* a file with
  a corrupt header instead of migrating it.
* `tests/test_capture_guard.py` — affinity lifecycle against stubbed Win32.
* `tests/test_esp_team.py` — label numbering/lifetime and tint palette.
* `tests/test_armor_tier.py` — tier anchors, blends, NaN/inf, packed colour.
* `tests/test_loot_esp.py` — owner allowlist, open-container rule, range,
  behind-camera, cache, and the exact packet trailer layout/size.
* `tests/test_mouse_output.py` — SendInput clicks, no `mouse_event` anywhere,
  a single injection helper, failed-send handling, noise off/on/NaN, and that
  only the aim call site can pass a non-zero amplitude.
* `tests/test_aim_modes.py`, `tests/test_aim_prediction.py`,
  `tests/test_runtime_tick.py` — harnesses extended for the new option word;
  their assertions are unchanged.

No game, anti-cheat, OBS, or hardware-capture validation is claimed here. A
compile and these isolated checks are not a live test.
