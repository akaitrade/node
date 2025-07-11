@echo off
REM CREDITS ALT-LEDGER 2030 - Debug Diagnostics
REM Comprehensive debugging and troubleshooting tool

setlocal enabledelayedexpansion

REM Default settings
set "ACTION=all"
set "VERBOSE=0"

REM Parse command line arguments
:parse_args
if "%~1"=="" goto :start_debug
if /I "%~1"=="--help" goto :show_help
if /I "%~1"=="-h" goto :show_help
if /I "%~1"=="--action" (
    set "ACTION=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--verbose" (
    set "VERBOSE=1"
    shift
    goto :parse_args
)
shift
goto :parse_args

:show_help
echo CREDITS ALT-LEDGER 2030 - Debug Diagnostics
echo.
echo Usage: debug.bat [OPTIONS]
echo.
echo Options:
echo   --action ^<name^>    Run specific debug action
echo   --verbose          Enable verbose output
echo   --help, -h         Show this help message
echo.
echo Actions:
echo   imports   - Debug import issues
echo   build     - Debug build problems
echo   libs      - Check library compilation
echo   deps      - Verify dependencies
echo   env       - Check environment setup
echo   all       - Run all diagnostics (default)
echo.
echo Examples:
echo   debug.bat                    # Run all diagnostics
echo   debug.bat --action imports   # Debug import issues only
echo   debug.bat --verbose          # Verbose output
exit /b 0

:start_debug
echo ==========================================
echo  CREDITS ALT-LEDGER 2030 - Debug Diagnostics
echo ==========================================
echo.

if /I "%ACTION%"=="all" goto :debug_all
if /I "%ACTION%"=="imports" goto :debug_imports
if /I "%ACTION%"=="build" goto :debug_build
if /I "%ACTION%"=="libs" goto :debug_libs
if /I "%ACTION%"=="deps" goto :debug_deps
if /I "%ACTION%"=="env" goto :debug_env
echo ERROR: Unknown action: %ACTION%
echo Use --help for available options
exit /b 1

:debug_all
call :debug_env
echo.
call :debug_deps
echo.
call :debug_libs
echo.
call :debug_build
echo.
call :debug_imports
echo.
echo ✅ Diagnostics complete!
exit /b 0

:debug_env
echo Checking Environment...
echo =======================
echo Rust version:
rustc --version
if errorlevel 1 (
    echo ❌ Rust not found! Install from https://rustup.rs/
    exit /b 1
)
cargo --version
echo.
echo Go version:
go version
if errorlevel 1 (
    echo ❌ Go not found! Install from https://golang.org/
    exit /b 1
)
echo.
echo Current directory: %CD%
echo.
echo ✅ Environment check passed!
exit /b 0

:debug_deps
echo Checking Dependencies...
echo ========================
cd /d "%~dp0..\core"

echo Rust dependencies:
cargo tree --depth 1 2>nul
if errorlevel 1 (
    echo ❌ Failed to check Rust dependencies
    echo Try running: cargo update
)

echo.
cd /d "%~dp0..\agent\go"
echo Go dependencies:
go mod graph 2>nul | find "github.com" | find /c /v "" > temp.txt
set /p GO_DEP_COUNT=<temp.txt
del temp.txt
echo Found %GO_DEP_COUNT% Go dependencies
if "%GO_DEP_COUNT%"=="0" (
    echo ❌ No Go dependencies found
    echo Try running: go mod tidy
)

cd /d "%~dp0"
echo ✅ Dependency check complete!
exit /b 0

:debug_libs
echo Checking Library Compilation...
echo ===============================
cd /d "%~dp0..\core"

echo Checking library only...
cargo check --lib 2>&1
if errorlevel 1 (
    echo ❌ Library compilation failed!
    echo This explains why binaries can't import types
    exit /b 1
)

echo.
echo Library exports:
cargo rustdoc -- --document-private-items 2>nul | find "pub" | find /c /v ""
echo.
echo ✅ Library compiles successfully!
cd /d "%~dp0"
exit /b 0

:debug_build
echo Checking Build Process...
echo =========================
cd /d "%~dp0..\core"

echo Build artifacts:
if exist target\release\dag_engine.dll (
    echo ✓ Found: dag_engine.dll
    dir target\release\dag_engine.dll | find "dag_engine.dll"
) else (
    echo ✗ Missing: dag_engine.dll
)

if exist target\release\credits-node.exe (
    echo ✓ Found: credits-node.exe
    dir target\release\credits-node.exe | find "credits-node.exe"
) else (
    echo ✗ Missing: credits-node.exe
)

echo.
echo Checking agent build:
cd /d "%~dp0..\agent\go"
if exist agent.exe (
    echo ✓ Found: agent.exe
    dir agent.exe | find "agent.exe"
) else (
    echo ✗ Missing: agent.exe
)

cd /d "%~dp0"
exit /b 0

:debug_imports
echo Debugging Import Issues...
echo ==========================
cd /d "%~dp0..\core"

echo Checking module structure...
echo.
echo Library modules in src\:
dir /b src\*.rs | find /v "main"
echo.

echo Checking lib.rs exports...
type src\lib.rs | find "pub mod" | find /c /v "" > temp.txt
set /p MODULE_COUNT=<temp.txt
del temp.txt
echo Found %MODULE_COUNT% public modules

type src\lib.rs | find "pub use" | find /c /v "" > temp.txt
set /p EXPORT_COUNT=<temp.txt
del temp.txt
echo Found %EXPORT_COUNT% public re-exports

echo.
echo Testing import resolution...
echo use dag_engine::*; > test_import.rs
echo fn main() {} >> test_import.rs
rustc --edition 2021 --explain E0432 >nul 2>&1
del test_import.rs

if "%VERBOSE%"=="1" (
    echo.
    echo Verbose: Checking Cargo.toml crate-type...
    type Cargo.toml | find "crate-type"
)

cd /d "%~dp0"
echo ✅ Import diagnostics complete!
exit /b 0