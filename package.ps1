param(
    [string]$Out = (Join-Path $PSScriptRoot 'dist'),
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$rel = Join-Path $root 'build\Release'
$needed = @('injector.exe', 'bsmap.exe', 'bs_pyexec.dll', 'bs_overlay.dll')

foreach ($f in $needed) {
    if (-not (Test-Path -LiteralPath (Join-Path $rel $f))) {
        throw "Missing build artifact: build\Release\$f (run cmake --build build --config Release)"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'pyexec\cmd.py'))) {
    throw 'Missing pyexec\cmd.py'
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'pyexec\bs_runtime.py'))) {
    throw 'Missing pyexec\bs_runtime.py'
}

if (Test-Path -LiteralPath $Out) {
    Remove-Item -LiteralPath $Out -Recurse -Force
}

$bin = Join-Path $Out 'build\Release'
$py = Join-Path $Out 'pyexec'
$ipc = Join-Path $Out 'ipc'
$drv = Join-Path $Out 'driver'
New-Item -ItemType Directory -Path $bin, $py, $ipc, $drv -Force | Out-Null

foreach ($f in $needed) {
    Copy-Item -LiteralPath (Join-Path $rel $f) -Destination $bin
}

Copy-Item -LiteralPath (Join-Path $root 'pyexec\cmd.py') -Destination $py
Copy-Item -LiteralPath (Join-Path $root 'pyexec\bs_runtime.py') -Destination $py
Copy-Item -LiteralPath (Join-Path $root 'run_cheat.bat') -Destination $Out
# The packed single-exe loader lands in dist/ (see CMakeLists.txt); accept
# either location so a fresh cmake build and a packed build both work.
$loaderExe = Join-Path $root 'dist\Comet.exe'
if (-not (Test-Path -LiteralPath $loaderExe)) { $loaderExe = Join-Path $rel 'Comet.exe' }
if (Test-Path -LiteralPath $loaderExe) {
    Copy-Item -LiteralPath $loaderExe -Destination (Join-Path $Out 'Comet.exe')
}

Copy-Item -LiteralPath (Join-Path $root 'driver\load_driver.bat') -Destination $drv
foreach ($f in @('bskdrv.sys', 'kdmapper.exe')) {
    $src = Join-Path $root "driver\$f"
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $drv
    }
}
$vuln = Join-Path $root 'driver\iqvw64e.sys'
if (-not (Test-Path -LiteralPath $vuln)) {
    $vuln = Join-Path (Split-Path $root -Parent) 'iqvw64e.sys'
}
if (Test-Path -LiteralPath $vuln) {
    Copy-Item -LiteralPath $vuln -Destination (Join-Path $drv 'iqvw64e.sys')
}

$readme = @'
Comet BloodStrike Internal - portable build

Option A (single file): run Comet.exe as administrator.
Option B: start BloodStrike.exe, wait for the lobby, then
          right-click run_cheat.bat -> Run as administrator.
Press INSERT in game to toggle the menu.

Layout:
  Comet.exe                    packed single-file loader
  build\Release\injector.exe   fallback injector
  build\Release\bsmap.exe      driver-mode mapper
  build\Release\bs_pyexec.dll  python bridge
  build\Release\bs_overlay.dll ImGui overlay
  pyexec\                      runtime scripts
  ipc\                         cfg/state/catalog (auto-created)
  driver\                      optional kernel driver files
  overlay_log.txt              created on first run

Driver mode is used automatically only when driver\bskdrv.sys,
driver\kdmapper.exe and build\Release\bsmap.exe all exist.
Otherwise the run_cheat.bat injector path is used.

Do not rename or move the folders; paths are resolved relative
to the DLLs.
'@
Set-Content -LiteralPath (Join-Path $Out 'README.txt') -Value $readme -Encoding ASCII

if ($Zip) {
    $zipPath = "$Out.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $zipPath
    Write-Output "Packaged: $zipPath"
} else {
    Write-Output "Packaged: $Out"
}
