@echo off
REM Clean build from scratch
REM Removes all build artifacts and rebuilds everything

echo Clean Build - Full Rebuild
echo ==========================
echo.

echo WARNING: This will delete all build artifacts!
echo Press Ctrl+C to cancel, or
pause

echo.
echo Cleaning DAG Engine...
cd /d "%~dp0..\core"
cargo clean
if exist target rmdir /s /q target 2>nul

echo Cleaning Agent Chain...
cd /d "%~dp0..\agent\go"
go clean -cache
if exist agent.exe del /f agent.exe
if exist *.out del /f *.out
if exist *.html del /f *.html

echo.
echo Clean complete. Starting fresh build...
echo.

REM Now run the full build
cd /d "%~dp0..\scripts"
call build.bat --phase 3
if errorlevel 1 (
    echo ❌ Build failed!
    cd /d "%~dp0"
    pause
    exit /b 1
)

echo.
echo ✅ Clean build complete!
cd /d "%~dp0"