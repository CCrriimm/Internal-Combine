$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "[!] vswhere.exe not found."
    exit 1
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    Write-Host "[!] Visual Studio C++ toolchain not found."
    exit 1
}

$msvcRoot = Join-Path $vs "VC\Tools\MSVC"
$msvcVersions = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending
if (-not $msvcVersions) {
    Write-Host "[!] No MSVC toolchain found in $msvcRoot"
    exit 1
}
$toolsDir = Join-Path $msvcVersions[0].FullName "bin\Hostx64\x64"
$cl = Join-Path $toolsDir "cl.exe"
$link = Join-Path $toolsDir "link.exe"
$libExe = Join-Path $toolsDir "lib.exe"

$libPath = Join-Path $root "ntoskrnl.lib"
$defPath = Join-Path $root "ntoskrnl.def"
if (Test-Path $libPath) {
    Remove-Item -Force $libPath
}
Write-Host "[i] generating ntoskrnl.lib from ntoskrnl.def..."
& $libExe /nologo "/def:$defPath" /machine:x64 "/out:$libPath"

Write-Host "[i] compiling bskdrv.c with standalone kernel headers..."
& $cl /nologo /c /O2 /GS- /Gs999999 /D_KERNEL_MODE /D_WIN64 /DAMD64 /utf-8 "$root\bskdrv.c" "/Fo$root\bskdrv.obj"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] compilation failed"
    exit $LASTEXITCODE
}

Write-Host "[i] linking bskdrv.sys..."
& $link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /NODEFAULTLIB "/OUT:$root\bskdrv.sys" "$root\bskdrv.obj" "$root\ntoskrnl.lib"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] link failed"
    exit $LASTEXITCODE
}

if (-not (Test-Path (Join-Path $root "bskdrv.sys"))) {
    Write-Host "[!] bskdrv.sys was not produced"
    exit 1
}

Write-Host "[OK] $root\bskdrv.sys built successfully."
