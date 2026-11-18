@echo off
rem Gamebreaker: opens the launcher, which sets up, builds and starts the game.
rem Gamebreaker.exe does the same; this file is the fallback if Windows will not
rem run that program. The launcher is launcher\prototype\host.ps1 (readable
rem PowerShell) showing launcher\mockups\halo.html. Without a window: setup\setup.cmd.
start "" conhost.exe --headless powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher\prototype\host.ps1" -Live %*
