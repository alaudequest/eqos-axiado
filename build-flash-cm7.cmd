@echo off
rem Build and flash only L0_MK_RMII_RX_PROMISCUOUS CM7.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-flash-cm7.ps1" -Project L0_MK_RMII_MAC_LOOPBACK %*
exit /b %errorlevel%
