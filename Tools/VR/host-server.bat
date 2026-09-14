@echo off
rem Starts the Skyrim Together server. Keep this file next to SkyrimTogetherServer.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0host-server.ps1" -ServerFolder "%~dp0."
pause
