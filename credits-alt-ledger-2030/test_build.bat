@echo off
echo === Testing Build Process ===
echo.

cd core

echo 1. Checking current directory:
cd
echo.

echo 2. Cleaning build:
cargo clean
echo.

echo 3. Checking main.rs for version marker:
findstr /C:"ENHANCED-2024-07-09-TRACKER" src\main.rs
echo.

echo 4. Building with verbose output:
cargo build --release --bin credits-node --verbose 2>&1  < /dev/null |  findstr /C:"main.rs"
echo.

echo 5. Running the built executable:
target\release\credits-node.exe --help | findstr /C:"VERSION"
echo.

echo 6. Running with cargo run:
cargo run --release --bin credits-node -- --help | findstr /C:"VERSION"
echo.

pause
