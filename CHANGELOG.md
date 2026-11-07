# Changelog

## Unreleased

- Remove the last `bs` names from anything that ships or is emitted: `bsmap.exe` -> `cometmap.exe`, `bs_overlay.dll` -> `comet_overlay.dll`, `bs_pyexec.dll` -> `comet_pyexec.dll`, and the build-only preview/test targets similarly. CMake target names are pinned with `OUTPUT_NAME` so `dist/Comet.exe` and the embedded payload entry names match what the loader extracts. `bskdrv.sys` keeps its name: it is a driver binary, not one of the shipped modules.
- Fix the frozen-picture report: the hook bound its own render target and never restored the game's, so the game kept drawing into the overlay's target with the wrong viewport while the window still took input. The previous RTV/DSV/viewport are now saved and restored around the ImGui render. `ResizeBuffers` also releases and re-creates the RTV explicitly and logs a failure code instead of silently leaving a stale back buffer.
- Fix the frame drops, all of them on the present path: `BindSwapchain` re-interrogated and *logged* (open/write/close of the log file) on every present of a non-selected swapchain and now caches rejections; config saving had two independent autosave paths, one rewriting both config files several times a second during a slider drag, and is now a single 250 ms path; the menu catalog hot-reload polled `ImGui::GetTime()` against a 1 s timer and is now a 2 s `GetTickCount64` check; the ESP text shadow stamped each glyph nine times and now draws two; the per-team tint built a fresh `unordered_set` per target and now builds it once per frame; targets fully above or below the viewport are skipped before any chip/bar work.
- Second aim keybind: `aim_key2` appended at the config tail (`Cfg` 224 -> 228, `VERSION` 8 -> 9, v8 files migrate forward), bound from Aim > Activation with a Clear button and an "unbound" hint, and honoured by the runtime's tracking and aim gates. Either key activates the aimbot.
- Loot ESP is now plain tinted dots with a dark rim - no text, so no text popping/flicker while the camera turns - and is capped at 96 items. Team box/snapline colours are assigned per team automatically (distinct, held for the whole match, never a colour the user already uses); the per-team pickers are gone. Hero name moved from the header to the feet row. The armour bar is coloured from the replicated max armour only, with the number label, the "max armor tier" toggle and the tier colour pickers removed.
- Rename the shipped build to **Comet**: the packed loader is `dist/Comet.exe`, the loader window/brand mark and config folder read Comet, and the Actions artifact is `comet-x64-release-<sha>`, still one zip from the same two directories so its contents are unchanged.
- Port the requested Comet features without touching existing behaviour: enemy hero-name chip (`hero_id` appended to the entity record), OBS capture protection via `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` with a timer-gated re-apply "bypass", ground-loot ESP (`CombatItemEntity`/`Supply`, packet trailer), max-armor tier colours (50 grey / 75 blue / 100 purple / 125 gold), compact team label plus per-team box and snapline colours, aimbot-only input noise, and SendInput as the runtime's only mouse-injection path (`mouse_event` and its fallback removed). See `docs/comet-port.md`.
- Config layout grows at the tail only (`Cfg` 160 -> 224 bytes, `VERSION` 7 -> 8, `Ent` 280 -> 288). A saved v7 `cfg.bin` is migrated forward, keeping every existing setting and filling the new tail with defaults, so a layout bump does not wipe anyone's preferences; `state.bin`/`catalog.bin` are runtime-owned and are rebuilt on the version check. Remove the unconditional `WDA_NONE` in `InitOverlay` so the capture guard owns display affinity.
- New isolated tests (capture guard, team label/tints, armor tiers, loot reader + packet trailer, mouse output) and the existing aim/tick harnesses updated for the new options word; 174 tests pass.

- Recognize remote CombatAvatar variants in custom rooms, prefer cur_maxhp for current maximum health, and resolve the active local combat avatar before stale avatar references. Add regression coverage; all 143 Python tests pass. The user confirmed the updated build works in game.
- Package the Release loader as one portable CiyE.exe with eight embedded payload files; verify embedded scripts match the runtime source.
- Add a saved FOV color picker to Aim, reusing an unused config slot without changing the 160-byte layout. Migrate older configs to the existing blue color. Keep the original FOV projection; the scaled-preview change was withdrawn at the user's request.
- Restore the original game WndProc when the menu closes and remove global GetAsyncKeyState/GetKeyState detours. Remove custom ShowCursor/clip restoration; use ImGui's drawn menu cursor and leave gameplay cursor behavior to the game. Native checks now verify real window-chain attach/detach across repeated menu transitions.
- Stop swallowing keyboard/raw keyboard merely because the menu is visible. Capture keyboard only for text entry/key binding, forward key releases, distinguish raw mouse from raw keyboard and clean up consumed raw input.
- Show the menu cursor on the game window thread with balanced ShowCursor adjustments; restore the original visibility/clip on close. Add actual WndProc routing checks for movement keys, Escape/backtick, text/bind capture and releases, plus cursor-state checks with stubbed OS calls.
- Temporarily disable Stream-proof at the user's request: ignore its saved flag, publish/save it off, and remove its activation control. Remove the forced gameplay cursor-hiding policy; return cursor handling to the game when the menu closes.
- Explicitly hide the OS cursor with the menu closed during a match, on both game and protected windows. Refresh the cursor on state changes without requiring mouse motion; preserve native cursor handling outside matches.
- Forward protected-window mouse events to the game in both menu states, and keep ImGui button releases working after closing. Use only the Windows cursor, restore the pre-menu cursor clip, and return cursor selection to the game. Draw ESP above the menu. Extend native checks with actual ImGui button activation through forwarded input.
- Fix Stream-proof menu mouse delivery across window threads. Stop repeatedly reclaiming the game's WndProc/raising the visual window; guard nested Present calls and reject foreign swap chains before frame/input work. Add native mouse-delivery checks. Discord coexistence still needs live verification.
- Fix the Stream-proof window's opaque redirection surface using a transparent composition-only window, and present before showing it. Route the complete ImGui frame, including the menu, exclusively to the protected output. Add GPU readback checks for transparent background and protected menu/ESP pixels.
- Match radar/offscreen-arrow switches to the other Visuals controls. Snaplines now end at the ESP box bottom for Bottom origin and box top for Top/Mid origins.
- Route Stream-proof ESP, snaplines, radar and aim FOV to a separate capture-excluded visual window; leave the game capturable. Add isolated native affinity/render/lifecycle checks. Requires borderless/windowed mode; live OBS verification remains pending.
- Defer Artifact equip and gun kill-feed/deathbox changes. Preserve the latest radar and snapline source in the packed build.

- Replace the inert Multipoint toggle/single-bone chooser with saved multi-select Aim points, evaluated Head > Neck > Body. Each selected point independently passes geometry/FOV/range and optional visibility checks before falling back.
- Visibility raycasts now use the requested point and point-specific cache keys; blocked-head results cannot mask a visible neck/body. Old bone/toggle configs migrate without changing the serialized layout.
- Export traceable effect/HUD/bundled-item references for the captured catalog and 15 saved Inventory skins. Kill VFX/deathbox roles remain unconfirmed; live read-only process access was denied.

- Use ImGui/D3D11 exclusively for the in-game menu; remove overlay D2D/DirectWrite build dependencies while preserving the loader byte-for-byte outside its payload.
- Refresh the menu with an isolated cyan/slate theme, grouped two-column controls, independently scrolling content, restored theme/Reduced Motion settings and correct Insert key labeling.
- Add an isolated native menu capture target with real default/compact render previews, without game startup or user config IO.

- Persist config separately from session IPC under LOCALAPPDATA/CiyE/BloodStrike/config.bin; migrate the prior IPC file when no persistent file exists, preserve invalid saved files until explicit Save, report failures and retry pending writes.
- Save UI theme/reduced motion, preserve Random bone selection, sanitize non-finite values and avoid replaying exit/apply commands on startup.
- Humanize now drives a 3–8-node Catmull-Rom path with a saved Human smoothing control, live endpoint tracking and resets on target/space changes or long frame gaps. Independent per-frame aim noise is removed.

- Aim selects candidates by center alignment (85%) and range (15%), favors enemies/standing targets and retains nearly equal current targets. Retention now obeys the same FOV, visibility, team and knock checks.
- ESP/aim candidates now require concrete player, robot, dummy or guard identity. Reject skill type markers/classes (fields, drones, holographic decoys, projectiles, traps, turrets and summons); generic combat units and monsters no longer qualify as player ESP.

- Aim Prediction now measures target velocity from monotonic position samples and applies lead to fresh and retained targets. Tracking lead follows camera smoothing; valid weapon projectile speed adds an intercept solution. Stale motion, teleports and space changes reset prediction.
- Exclude Magic Field objects by IsMagicField and class ancestry before accepting combat flags; keep player avatars affected by field skills.

- ESP now requires explicit combat-avatar/monster identity and always excludes vehicles; player occupants remain eligible.
- Team checks normalize IDs, prefer same-team/IsTeammate over friendly-fire CanDamage, and apply to bots too. Unknown player relationships remain hidden with teammates off.
- Visuals exposes Enable ESP and Show teammates near the top; its body scrolls so lower settings are reachable. Counter filtering follows Show teammates.
- Aim adds a saved Ignore knocked targets option (off by default), including the retained-target path and knock transitions after gathering.

- Show the four Artifact melee entries in a reference-only Artifacts category; Apply remains disabled until multipart Artifact equipment is supported.
- Show only the highest valid permanent level per gun upgrade family; retain old template mappings for saved selections.

- Inventory now contains equipped selections only; old inactive history is removed when loading.
- Clicking a skin name or Apply in Characters, Knives or Guns immediately replaces and saves its slot. Removed the redundant Inventory Apply action.

## v2.1.0-stable — 2026-11-05

Stable checkpoint, based on the user's confirmation that the current version works in game.

- Added Inventory next to Characters, Knives and Guns.
- Persist Apply selections, retain history, and select the latest skin per hero/weapon group.
- Requeue compatible saved skins as combat avatars, equipment and sessions change, including training sessions represented by combat avatars.
- Resolve overlapping melee selections by recency; preserve saved files on read/write errors.
- Refined the menu and loader to the supplied Roblox visual reference; omitted the sidebar status/footer as requested.
- Preserved separate main and dist runtime sources and added isolated cosmetic regression coverage.

Validation: 40 offline tests passed for each runtime. User-reported in-game success is recorded separately from the agent's tests. No new application build, binary release or independent in-game validation was performed by the agent.

Known scope: matching hero, compatible melee class and available model resources are required. Artifact cosmetics need more than a single ID. The old update_runtime.py generator may overwrite edited runtime functions from its .bak input if run manually.
