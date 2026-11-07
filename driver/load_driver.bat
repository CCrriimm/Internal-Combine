@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set COMETMAP=..\build\Release\cometmap.exe
set SYS=bskdrv.sys
set KDMAPPER=kdmapper.exe
set VULN=iqvw64e.sys

if not exist "%COMETMAP%" (
  if exist "cometmap.exe" set COMETMAP=cometmap.exe
  if exist "%~dp0..\build\Release\cometmap.exe" set COMETMAP=%~dp0..\build\Release\cometmap.exe
  if exist "%~dp0..\cometmap.exe" set COMETMAP=%~dp0..\cometmap.exe
)

"%COMETMAP%" --probe >nul 2>&1
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
"%COMETMAP%" --probe >nul 2>&1
if not errorlevel 1 (
  echo [OK] driver loaded.
  exit /b 0
)
timeout /t 2 /nobreak >nul
"%COMETMAP%" --probe >nul 2>&1
if not errorlevel 1 (
  echo [OK] driver loaded.
  exit /b 0
)

echo [!] driver load failed.
echo     - Run as Administrator
echo     - Disable HVCI / Memory Integrity, or use a kdmapper build that supports it
echo     - Check that %VULN% matches your kdmapper version
exit /b 1
