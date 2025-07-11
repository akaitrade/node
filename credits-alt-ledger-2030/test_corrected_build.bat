@echo off
REM Test corrected build - preserving original working code

echo Testing Corrected Build...
cd /d "%~dp0"
cd core

echo Step 1: Clean build
echo ===================
cargo clean

echo Step 2: Check library compilation
echo ==================================
cargo check --lib
if errorlevel 1 (
    echo ERROR: Library check failed
    pause
    exit /b 1
)

echo Step 3: Build library
echo ====================
cargo build --release --lib
if errorlevel 1 (
    echo ERROR: Library build failed
    pause
    exit /b 1
)

echo Step 4: Check simple binary
echo =============================
cargo check --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple binary check failed
    pause
    exit /b 1
)

echo Step 5: Check main binary
echo ============================
cargo check --bin credits-node
if errorlevel 1 (
    echo ERROR: Main binary check failed - this will show the exact error
    pause
    exit /b 1
)

echo Step 6: Build simple binary
echo ============================
cargo build --release --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple binary build failed
    pause
    exit /b 1
)

echo Step 7: Build main binary
echo ===========================
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Main binary build failed
    echo The library is working, but main.rs has import issues
    pause
    exit /b 1
)

echo SUCCESS: All builds completed!
echo.
echo Available executables:
echo   - core\target\release\simple-node.exe (basic test)
echo   - core\target\release\credits-node.exe (full DAG node)
echo.
echo Testing executables...
echo.

echo Simple node test:
echo ==================
core\target\release\simple-node.exe --help 2>nul
if errorlevel 1 (
    echo Simple node doesn't have --help, but it should run interactively
)

echo.
echo DAG node test:
echo ==============
core\target\release\credits-node.exe --help
if errorlevel 1 (
    echo DAG node doesn't have --help, but it should run interactively
)

echo.
echo Build complete!
pause