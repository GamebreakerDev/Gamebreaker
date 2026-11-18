@echo off
rem Gamebreaker setup. Usage:  setup.cmd "D:\path\to\your disc.iso"
rem Everything it does is in setup.ps1 next to this file (readable PowerShell).
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
exit /b %errorlevel%
