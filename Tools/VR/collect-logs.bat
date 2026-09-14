@echo off
rem Zips the Skyrim Together VR logs onto the Desktop. Keep this file in the "Skyrim Together VR" tools folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0collect-logs.ps1" -ClientFolder "%~dp0."
pause
