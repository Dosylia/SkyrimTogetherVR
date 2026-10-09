@echo off
rem Zips the urSovngarde logs onto the Desktop. Keep this file in the mod's folder, next to urSovngarde.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-logs.ps1" -ClientFolder "%~dp0."
pause
