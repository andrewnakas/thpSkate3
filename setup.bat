@echo off
rem One-time setup from your own games: setup.bat THPS.z64 SKATE3.iso
rem Runs tools/setup.sh with Git for Windows' bash (needs Git for Windows, Python 3 and uv).
setlocal
set "BASH="
for %%B in ("%ProgramFiles%\Git\bin\bash.exe" "%ProgramFiles(x86)%\Git\bin\bash.exe" "%LocalAppData%\Programs\Git\bin\bash.exe") do (
    if not defined BASH if exist %%B set "BASH=%%~B"
)
if not defined BASH (
    echo Git for Windows is needed for setup: https://git-scm.com/download/win
    pause
    exit /b 1
)
if "%~2"=="" (
    echo usage: setup.bat "THPS.z64" "SKATE3.iso"
    pause
    exit /b 2
)
"%BASH%" "%~dp0tools\setup.sh" "%~f1" "%~f2"
if errorlevel 1 (
    echo Setup failed.
    pause
    exit /b 1
)
echo Setup done. Start the game with thpskate3.bat
pause
