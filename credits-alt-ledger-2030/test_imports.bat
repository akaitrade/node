@echo off
REM Test imports fix

echo Testing import fix...
cd /d "%~dp0"
cd core

echo Checking main binary compilation...
cargo check --bin credits-node
if errorlevel 1 (
    echo ERROR: Still has import issues
    pause
    exit /b 1
)

echo SUCCESS: Import issues resolved!
echo.
echo Building main binary...
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Build failed after import fix
    pause
    exit /b 1
)

echo SUCCESS: DAG node built successfully!
echo.
echo Available executables:
echo   - core\target\release\simple-node.exe
echo   - core\target\release\credits-node.exe
echo.
pause