@echo off
setlocal EnableExtensions
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo [i] Administrator required. Requesting elevation...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
  exit /b 0
)

set ROOT=%~dp0
set INJ=%ROOT%build\Release\injector.exe
set BSMAP=%ROOT%build\Release\bsmap.exe
set PYEXEC=%ROOT%build\Release\bs_pyexec.dll
set OVERLAY=%ROOT%build\Release\bs_overlay.dll
set INJLOG=%ROOT%build\Release\inject_log.txt

if not exist "%PYEXEC%" goto missing
if not exist "%OVERLAY%" goto missing
if not exist "%ROOT%pyexec\cmd.py" goto missing
if not exist "%ROOT%pyexec\bs_runtime.py" goto missing

set DRIVERMODE=0
if exist "%ROOT%driver\kdmapper.exe" if exist "%ROOT%driver\bskdrv.sys" if exist "%BSMAP%" set DRIVERMODE=1

echo [0/4] waiting for BloodStrike.exe...
set GAMEFOUND=0
for /l %%i in (1,1,60) do (
  tasklist /fi "imagename eq BloodStrike.exe" | find /i "BloodStrike.exe" >nul
  if not errorlevel 1 (
    set GAMEFOUND=1
    goto gamefound
  )
  timeout /t 2 /nobreak >nul
)
:gamefound
if "%GAMEFOUND%"=="0" (
  echo [!] BloodStrike.exe is not running after 120 seconds.
  echo     Start the game, wait for the lobby, then run this again.
  pause
  exit /b 1
)

if not "%DRIVERMODE%"=="1" goto fallback

echo [1/4] loading kernel driver...
call "%ROOT%driver\load_driver.bat"
if errorlevel 1 goto driverfailed

echo [2/4] mapping python bridge...
"%BSMAP%" BloodStrike.exe "%PYEXEC%"
if errorlevel 1 goto driverfailed
timeout /t 2 /nobreak >nul

echo [3/4] mapping overlay...
"%BSMAP%" BloodStrike.exe "%OVERLAY%"
if errorlevel 1 (
  echo [!] Overlay mapping failed. The python bridge is already loaded;
  echo     close the game before retrying.
  pause
  exit /b 3
)

echo [4/4] done (driver mode).
goto ok

:driverfailed
echo [!] Driver mode failed / unavailable. Falling back to injector.
set DRIVERMODE=0

:fallback
if not exist "%INJ%" goto missing
echo [1/3] injecting python bridge...
"%INJ%" BloodStrike.exe "%PYEXEC%" --wait 30
if errorlevel 1 goto failed
timeout /t 2 /nobreak >nul

echo [2/3] injecting overlay...
"%INJ%" BloodStrike.exe "%OVERLAY%" --wait 30
if errorlevel 1 goto failed

echo [3/3] done (injector mode).
goto ok

:ok
echo.
echo [OK] Injected as Administrator.
echo      Press INSERT in game to toggle the menu.
echo      Mode       : %DRIVERMODE% (1 = kernel driver)
echo      Inject log : %INJLOG%
echo      Overlay log: %ROOT%overlay_log.txt
echo      Runtime log: %ROOT%ipc\runtime_err.txt
echo      Boot file  : %ROOT%pyexec\booted.txt
pause
exit /b 0

:missing
echo [!] Required files are missing.
echo     Keep run_cheat.bat next to build\, pyexec\ and ipc\.
echo     Rebuild with:
echo       cmake -S "%ROOT%." -B "%ROOT%build"
echo       cmake --build "%ROOT%build" --config Release
pause
exit /b 2

:failed
echo [!] Injection failed.
echo     Check: %INJLOG%
echo     Common causes:
echo       - Game is protected / running as another user
echo       - Antivirus blocked the DLL
echo       - Wrong game build / outdated Python offsets
pause
exit /b 3
