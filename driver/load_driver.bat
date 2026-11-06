@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set BSMAP=..\build\Release\bsmap.exe
set SYS=bskdrv.sys
set KDMAPPER=kdmapper.exe
set VULN=iqvw64e.sys

if not exist "%BSMAP%" (
  if exist "bsmap.exe" set BSMAP=bsmap.exe
  if exist "%~dp0..\build\Release\bsmap.exe" set BSMAP=%~dp0..\build\Release\bsmap.exe
  if exist "%~dp0..\bsmap.exe" set BSMAP=%~dp0..\bsmap.exe
)

"%BSMAP%" --probe >nul 2>&1
if not errorlevel 1 (
  echo [i] driver already loaded.
  exit /b 0
)

if not exist "%SYS%" (
  echo [!] %SYS% missing. Build the driver with build_driver.ps1 first.
  exit /b 1
)
if not exist "%KDMAPPER%" (
  echo [!] kdmapper.exe missing. Put kdmapper.exe in %CD%.
  exit /b 1
)
if not exist "%VULN%" (
  if exist "..\..\iqvw64e.sys" copy /y "..\..\iqvw64e.sys" . >nul
  if exist "..\iqvw64e.sys" copy /y "..\iqvw64e.sys" . >nul
)

echo [i] loading %SYS% with kdmapper...
"%KDMAPPER%" "%SYS%"
timeout /t 2 /nobreak >nul
"%BSMAP%" --probe >nul 2>&1
if not errorlevel 1 (
  echo [OK] driver loaded.
  exit /b 0
)
timeout /t 2 /nobreak >nul
"%BSMAP%" --probe >nul 2>&1
if not errorlevel 1 (
  echo [OK] driver loaded.
  exit /b 0
)

echo [!] driver load failed.
echo     - Run as Administrator
echo     - Disable HVCI / Memory Integrity, or use a kdmapper build that supports it
echo     - Check that %VULN% matches your kdmapper version
exit /b 1
