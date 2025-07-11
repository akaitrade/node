@echo off
REM Test simple build script for Windows

echo Testing Simple Node Build...
cd /d "%~dp0"
cd core

echo Building simple node first...
cargo build --release --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple node build failed
    pause
    exit /b 1
)

echo SUCCESS: Simple node built!
echo Testing simple node...
echo.
echo You can now run:
echo   core\target\release\simple-node.exe
echo.

echo Attempting full DAG node build...
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Full DAG node build failed
    echo But simple node is working!
    pause
    exit /b 1
)

echo SUCCESS: Both nodes built successfully!
echo.
echo Available executables:
echo   - core\target\release\simple-node.exe (basic functionality)
echo   - core\target\release\credits-node.exe (full DAG engine)
echo.
pause