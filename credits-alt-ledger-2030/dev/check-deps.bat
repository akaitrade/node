@echo off
REM Dependency checker with optional auto-install
REM Checks and optionally installs missing dependencies

setlocal enabledelayedexpansion

echo Dependency Checker
echo ==================
echo.

set "INSTALL=0"
if /I "%~1"=="--install" set "INSTALL=1"
if /I "%~1"=="--help" goto :show_help

:show_help
if /I "%~1"=="--help" (
    echo Usage: check-deps.bat [OPTIONS]
    echo.
    echo Options:
    echo   --install    Automatically install missing dependencies
    echo   --help       Show this help message
    echo.
    exit /b 0
)

set "MISSING=0"

echo Checking Rust toolchain...
rustc --version >nul 2>&1
if errorlevel 1 (
    echo ❌ Rust not found!
    set "MISSING=1"
    if "%INSTALL%"=="1" (
        echo Installing Rust...
        curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
        call "%USERPROFILE%\.cargo\env.bat"
    ) else (
        echo Install from: https://rustup.rs/
    )
) else (
    echo ✅ Rust found: 
    rustc --version
)

echo.
echo Checking Go toolchain...
go version >nul 2>&1
if errorlevel 1 (
    echo ❌ Go not found!
    set "MISSING=1"
    if "%INSTALL%"=="1" (
        echo Please install Go manually from: https://golang.org/dl/
    ) else (
        echo Install from: https://golang.org/dl/
    )
) else (
    echo ✅ Go found:
    go version
)

echo.
echo Checking optional tools...

REM Check for cargo-tarpaulin (code coverage)
cargo tarpaulin --version >nul 2>&1
if errorlevel 1 (
    echo ⚠️  cargo-tarpaulin not found (optional - for code coverage)
    if "%INSTALL%"=="1" (
        echo Installing cargo-tarpaulin...
        cargo install cargo-tarpaulin
    )
) else (
    echo ✅ cargo-tarpaulin found
)

REM Check for sccache (build cache)
sccache --version >nul 2>&1
if errorlevel 1 (
    echo ⚠️  sccache not found (optional - speeds up builds)
    if "%INSTALL%"=="1" (
        echo Installing sccache...
        cargo install sccache
    )
) else (
    echo ✅ sccache found
)

echo.
echo Checking Rust dependencies...
cd /d "%~dp0..\core"
cargo check --quiet 2>nul
if errorlevel 1 (
    echo ⚠️  Some Rust dependencies may be missing
    if "%INSTALL%"=="1" (
        echo Running cargo update...
        cargo update
        cargo fetch
    )
) else (
    echo ✅ Rust dependencies OK
)

echo.
echo Checking Go dependencies...
cd /d "%~dp0..\agent\go"
go mod verify >nul 2>&1
if errorlevel 1 (
    echo ⚠️  Go module verification failed
    if "%INSTALL%"=="1" (
        echo Running go mod tidy...
        go mod tidy
        go mod download
    )
) else (
    echo ✅ Go dependencies OK
)

cd /d "%~dp0"
echo.
if "%MISSING%"=="1" (
    echo ❌ Some required dependencies are missing!
    if "%INSTALL%"=="0" (
        echo Run with --install to attempt automatic installation
    )
    exit /b 1
) else (
    echo ✅ All required dependencies are installed!
)
exit /b 0