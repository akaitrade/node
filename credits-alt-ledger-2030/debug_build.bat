@echo off
REM Debug build script for Windows

echo Testing incremental build...
cd /d "%~dp0"
cd core

echo Step 1: Check basic compilation
echo ================================
cargo check --lib
if errorlevel 1 (
    echo ERROR: Library compilation failed
    pause
    exit /b 1
)

echo Step 2: Check simple binary
echo =============================
cargo check --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple binary compilation failed
    pause
    exit /b 1
)

echo Step 3: Check full binary
echo ===========================
cargo check --bin credits-node
if errorlevel 1 (
    echo ERROR: Full binary compilation failed
    echo This will show the exact error
    pause
    exit /b 1
)

echo Step 4: Build simple binary
echo ============================
cargo build --release --bin simple-node
if errorlevel 1 (
    echo ERROR: Simple binary build failed
    pause
    exit /b 1
)

echo Step 5: Build full binary
echo ==========================
cargo build --release --bin credits-node
if errorlevel 1 (
    echo ERROR: Full binary build failed
    pause
    exit /b 1
)

echo SUCCESS: All builds completed!
echo.
echo Available executables:
echo   - core\target\release\simple-node.exe
echo   - core\target\release\credits-node.exe
echo.
pause