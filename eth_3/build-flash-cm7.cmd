@echo off
rem Build and flash only eth_3 CM7.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build-flash-cm7.ps1" -Project eth_3 %*
exit /b %errorlevel%
