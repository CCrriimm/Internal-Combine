@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_driver.ps1"
pause
