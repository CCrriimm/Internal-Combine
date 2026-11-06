# Runtime skin-lookup performance fix

Measured on 2026-11-05 in one running BloodStrike session. Frame logs showed 300-460 ms in the apply/skin block while entity gathering took about 1-2 ms. Five-second helper timing isolated repeated catalog scans for missing or outdated saved templates.

## Changes

- Cache missing template lookups as well as successful results. Invalidate the cache when the catalog object or its size changes, and retain upgrade-priority mappings prepared during catalog indexing.
- Filter Inventory records against equipped weapon/melee groups before resolving templates.
- Remove a recently added 330 ms skin-driver throttle after fixing the expensive lookup. Immediate equipment/session/Apply transitions remain supported; actual application tasks retain their existing rate limit.
- Replace an undefined t_aim reference in end-of-tick diagnostics with the measured tracking interval. The original code emitted repeated NameError tracebacks.

## Measurements

| Python runtime callback metric | Before | After |
|---|---:|---:|
| Observation window | 5.006 s | 5.003 s |
| Completed callbacks | 60 | 1,186 |
| Mean callback duration | 76.65 ms | 1.94 ms |
| Maximum callback duration | 364.54 ms | 3.82 ms |
| Template lookups | 4.469 s / 179 calls | 0.072 s / 4,748 calls |
| Maximum skin-driver duration | 362.60 ms | 0.79 ms |

These are wall-clock timings under lightweight Python wrappers, not GPU frame-time or FPS measurements. They do not establish identical results on every map or player count.

E-before: before.json. E-after: after.json. E-patch: patch.json records replaced helpers and the historical input-source hash. Both timing passes restored the wrapped functions, found no pending recoil probe, and found all sys.monitoring tool slots free.

Finding: the original path runtime tick -> saved-skin driver -> Inventory target resolution -> template lookup repeatedly scanned the catalog on misses. The after-capture confirms that the hot skin-driver path dropped below 1 ms at its measured maximum. The checkpoint passed 145 isolated tests, including cache misses, invalidation, unloaded equipment, state transitions and the fast diagnostics path. Later revisions add further tests.
