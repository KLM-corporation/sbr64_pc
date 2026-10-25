@echo off
rem Copies the game files from your APK and OBB files into data\ (asks for their location).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\import_game_data.ps1" %*
pause
