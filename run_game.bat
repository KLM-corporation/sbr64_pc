@echo off
cd /d "%~dp0"
if exist "build\sbr64_pc.exe" (
    "build\sbr64_pc.exe" %*
) else (
    "sbr64_pc.exe" %*
)
