@echo off
REM Test fixed build script for Windows

echo Testing Fixed DAG Node Build...
cd /d "%~dp0"
cd core

echo Attempting full DAG node build...
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Full DAG node build failed
    echo Let's try to see what specific error we get
    pause
    exit /b 1
)

echo SUCCESS: DAG node built successfully!
echo.
echo Available executables:
echo   - core\target\release\simple-node.exe (basic functionality)
echo   - core\target\release\credits-node.exe (full DAG engine)
echo.
echo Testing the executables...
echo.

echo Testing simple node help:
echo =============================
core\target\release\simple-node.exe --help
if errorlevel 1 (
    echo Note: Simple node might not have --help flag
)

echo.
echo Testing DAG node help:
echo =====================
core\target\release\credits-node.exe --help
if errorlevel 1 (
    echo Note: DAG node might not have --help flag
)

echo.
echo Build complete! You can now run:
echo   1. core\target\release\simple-node.exe
echo   2. core\target\release\credits-node.exe
echo.
pause