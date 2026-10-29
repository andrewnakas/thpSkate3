@echo off
rem Start thpSkate3 (run setup.bat THPS.z64 SKATE3.iso once first).
cd /d "%~dp0"
if not exist "work\thps.z64" (
    echo Run setup.bat "THPS.z64" "SKATE3.iso" first.
    pause
    exit /b 1
)
"%~dp0port\build\THPS64Recomp.exe" "%~dp0work\thps.z64" %*
