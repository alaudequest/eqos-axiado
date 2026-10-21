@echo off
rem Build and flash only L0_MK_RMII_MAIN_FILE CM7.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build-flash-cm7.ps1" -Project L0_MK_RMII_MAIN_FILE %*
exit /b %errorlevel%
