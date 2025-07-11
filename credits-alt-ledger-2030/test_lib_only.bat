@echo off
REM Test just the library compilation to see what's actually working

echo Testing Library-Only Build...
cd /d "%~dp0"
cd core

echo Step 1: Clean everything
echo =========================
cargo clean

echo Step 2: Check library only
echo ==========================
cargo check --lib
if errorlevel 1 (
    echo ERROR: Library itself has compilation errors
    echo This would explain why modules aren't available
    pause
    exit /b 1
)
echo SUCCESS: Library compiles

echo Step 3: Build library only
echo ==========================
cargo build --release --lib
if errorlevel 1 (
    echo ERROR: Library build failed
    pause
    exit /b 1
)
echo SUCCESS: Library builds

echo Step 4: List what was actually built
echo ====================================
dir target\release\
if exist target\release\libdag_engine.rlib (
    echo Found: libdag_engine.rlib
)
if exist target\release\dag_engine.dll (
    echo Found: dag_engine.dll
)
if exist target\release\dag_engine.lib (
    echo Found: dag_engine.lib
)

echo Step 5: Try to use the library in a simple way
echo ===============================================
echo This will show if the library exports are working
pause