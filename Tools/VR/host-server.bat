@echo off
rem Starts the urSovngarde server. Keep this file next to urSovngardeServer.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0host-server.ps1" -ServerFolder "%~dp0."
pause
