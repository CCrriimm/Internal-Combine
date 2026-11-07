# Comet

Windows x64 C++/Python client source and game-logic research.

The build is named **Comet** everywhere it is user visible: the packed loader is
`dist/Comet.exe`, the window title and brand mark read Comet, and its support
files live under `%TEMP%\Comet_BSLoader`. The build still produces the same
outputs as before, in the same single artifact (see *Build outputs*).

This snapshot includes runtime performance fixes, appearance/effect bindings and UI changes recorded on **2026-11-05**. Reported in-game confirmation is separate from the isolated regression checks documented with each feature.

This checkpoint includes the revised Comet menu and loader presentation, permanent high-tier cosmetic selection, and the Inventory tab. Inventory shows equipped selections only. Clicking a skin name or Apply in Characters, Knives or Guns immediately replaces and saves its slot. Compatible saved skins are scheduled again when combat equipment or sessions change; inactive history from older files is removed.

Validation details and measured limits are recorded with each logic report. Unit tests extract runtime functions with mocked game objects; they do not establish compatibility for every game mode or skin.

Git tracks source, tests, vendored source dependencies, and reports/evidence about game logic and functions. Generated binaries, distribution copies, build/packaging manifests, local IPC state, logs, archives and credentials are excluded. Vendored dependencies retain their upstream license files. No public license is granted for project code.

The separately cloned driver/kdmapper_src repository is excluded rather than recorded as an unresolved Git link. This checkpoint does not package external mapper/driver tools.

Details: [Inventory](docs/skin-inventory.md), [release notes](CHANGELOG.md), [design](DESIGN.md).

Comet port additions (enemy hero name, OBS capture protection and its re-apply
bypass, loot ESP, max-armor tier colours, team label + team colours, aimbot-only
input noise, SendInput-only mouse output) are documented in
[docs/comet-port.md](docs/comet-port.md).

Capture protection - what each capture path reads, which one OBS Game Capture
and Steam use, and what this build does about them - is in
[docs/capture-protection.md](docs/capture-protection.md).

Run the isolated regression suite with `python -m unittest discover -s tests`.

Logic reports: [skin effects](reports/weapon-effects/2026-11-05_skin-effects-fix-report.md), [recoil](reports/recoil-probe/2026-11-05_recoil-report.md), [runtime performance](reports/performance/2026-11-05_runtime-lag-report.md), and [custom-room fields](reports/custom-room/2026-11-05_custom-room-report.md). The full weapon-effect mapping is in `reports/weapon-effects/weapon-effects.json` and `all-menu-guns.csv`.

Offset extraction: `tools/offset_dump.py`, with SDK/header inputs under `reference/offsets` and recorded results under `reports/offsets`.

## Build and source layout

The C++ targets require Windows, CMake and the MSVC C++20 toolchain. Python tools use repository-relative inputs; runtime probes depend on the embedded Python ABI of the examined game build.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target comet_loader comet_pyexec comet_overlay
```

### Build outputs

GitHub Actions still uploads one artifact zip from the same two directories, so
its contents are unchanged apart from the loader's name:

```
dist/Comet.exe                        packed single-file loader
build/Release/injector.exe            fallback injector
build/Release/cometmap.exe            driver-mode mapper
build/Release/comet_pyexec.dll        Python bridge payload
build/Release/comet_overlay.dll       overlay payload
```

No shipped file starts with `bs` any more. The loader extracts its embedded
payload by the same names the packer embeds (`tools/pack_loader.py --file`), so
the CMake targets pin those names with `OUTPUT_NAME` rather than relying on the
target name. The IPC shared-memory names (`Local\BS_IPC_STATE`,
`Local\BS_IPC_CFG`) are internal to the payload trio and are unchanged: all
three must agree on them, and they are not user visible.

- `src`: loader, overlay, configuration and native geometry.
- `pyexec`: embedded-Python runtime and its entry point.
- `tools`: offset extraction, catalog export and runtime observation tools.
- `tests`: isolated Python/native regression harnesses.
- `reference`: SDK, captured catalog inputs and UI reference source.
- `reports`: function analysis, bytecode, field mappings and performance evidence.
- `third_party`: vendored dependencies with upstream licenses.

See [source distribution](docs/source-distribution.md) for the English-source and normalized-path conventions. The source package contains no Git history or compiled distribution.
