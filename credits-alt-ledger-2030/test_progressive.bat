@echo off
REM Test progressive builds to find the issue

echo Testing Progressive Builds...
cd /d "%~dp0"
cd core

echo Step 1: Test minimal node (no imports)
echo =======================================
cargo build --release --bin minimal-node
if errorlevel 1 (
    echo ERROR: Even minimal node failed
    pause
    exit /b 1
)
echo SUCCESS: Minimal node builds

echo Step 2: Test node with basic imports
echo =====================================
cargo build --release --bin test-node
if errorlevel 1 (
    echo ERROR: Basic imports failed
    echo This shows which imports are broken
    pause
    exit /b 1
)
echo SUCCESS: Basic imports work

echo Step 3: Test simple node (JSON only)
echo =====================================
cargo build --release --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple node failed
    pause
    exit /b 1
)
echo SUCCESS: Simple node builds

echo Step 4: Test full node (all imports)
echo ====================================
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Full node failed
    echo But we know basic imports work
    pause
    exit /b 1
)
echo SUCCESS: Full node builds

echo All tests passed!
echo.
echo Available executables:
echo   - core\target\release\minimal-node.exe
echo   - core\target\release\test-node.exe
echo   - core\target\release\simple-node.exe
echo   - core\target\release\credits-node.exe
echo.
pause