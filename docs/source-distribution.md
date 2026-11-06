# Source distribution

Project-owned documentation, report narratives, comments and interface text are written in English. Third-party files retain their upstream text and licenses. Captured game strings, API identifiers, bytecode constants and resource paths are preserved as evidence; translating those values would change the data being analyzed.

The source ZIP contains project source, vendored source dependencies, tests, portable tools, SDK/catalog inputs and logic reports. It excludes Git history, build outputs, distribution copies, local IPC/configuration, logs, credentials, temporary captures and packaging manifests. The original full-workspace backup is a separate artifact and is not the sanitized source package.

## Path handling

- Repository inputs use relative paths under reference or reports.
- The offset tool requires an explicit --exe argument rather than assuming an installation directory.
- Windows font lookup uses GetWindowsDirectoryA. Native test compiler discovery uses the ProgramFiles environment variables.
- Runtime preferences and temporary extraction use operating-system locations. Paths such as %LOCALAPPDATA% and %TEMP% describe portable locations, not a particular user profile.
- Distributed capture metadata uses normalized logical paths. New local captures can be written under reports/local, which is excluded from source control and the source package.

The distributed SHA-256 inventories cover the sanitized artifact bytes. Historical source hashes stored inside captures still identify the source used during that original observation, rather than the current translated source. Report findings, numeric observations and original game data are retained; path normalization does not add new runtime validation.

## Reproduction

```powershell
python -m unittest discover -s tests
$game = Get-Process BloodStrike | Select-Object -First 1
python tools/offset_dump.py --exe $game.Path --out reports/local/offsets
python tools/export_skin_effect_refs.py --out reports/local/skin-references
```

The offset command needs read access to the installed executable. Native regression tests require the MSVC C++ toolchain; game runtime probes additionally require Frida and a compatible running game build. These tools do not make a captured offset or function signature portable across game updates.
