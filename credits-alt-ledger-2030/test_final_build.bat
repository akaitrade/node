@echo off
REM Final test build - using correct crate imports

echo Testing Final Build...
cd /d "%~dp0"
cd core

echo Checking main binary compilation...
cargo check --bin credits-node
if errorlevel 1 (
    echo ERROR: Main binary check failed
    echo This shows the exact compilation error
    pause
    exit /b 1
)

echo Building main binary...
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Main binary build failed
    pause
    exit /b 1
)

echo SUCCESS: DAG node built!
echo.
echo Testing the executable...
echo.

echo Testing help flag:
core\target\release\credits-node.exe --help
if errorlevel 1 (
    echo Note: May not have --help flag, but should run
)

echo.
echo Build complete! 
echo Available executables:
echo   - core\target\release\simple-node.exe (basic test)
echo   - core\target\release\credits-node.exe (full DAG node)
echo.
echo To run the DAG node:
echo   core\target\release\credits-node.exe
echo.
pause