@echo off
rem Starts the Windows build you made (make windows or make windows-zig).
cd /d "%~dp0"
set EXE=
if exist "build\windows\ds2rb.exe" set EXE=build\windows\ds2rb.exe
if exist "build\windows-zig\ds2rb.exe" set EXE=build\windows-zig\ds2rb.exe
if "%EXE%"=="" (
    echo No Windows build found. See README.md: make setup recomp windows
    pause
    exit /b 1
)
"%EXE%" --data "%~dp0data" %*
if errorlevel 1 pause
