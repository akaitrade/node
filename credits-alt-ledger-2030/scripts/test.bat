@echo off
REM CREDITS ALT-LEDGER 2030 - Unified Test Runner
REM Run tests for all components with a single command

setlocal enabledelayedexpansion

REM Default settings
set "COMPONENT=all"
set "VERBOSE=0"
set "COVERAGE=0"

REM Parse command line arguments
:parse_args
if "%~1"=="" goto :start_tests
if /I "%~1"=="--help" goto :show_help
if /I "%~1"=="-h" goto :show_help
if /I "%~1"=="--component" (
    set "COMPONENT=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--verbose" (
    set "VERBOSE=1"
    shift
    goto :parse_args
)
if /I "%~1"=="--coverage" (
    set "COVERAGE=1"
    shift
    goto :parse_args
)
shift
goto :parse_args

:show_help
echo CREDITS ALT-LEDGER 2030 - Test Runner
echo.
echo Usage: test.bat [OPTIONS]
echo.
echo Options:
echo   --component ^<name^>  Test specific component (dag, agent, all)
echo   --verbose          Enable verbose test output
echo   --coverage         Generate coverage reports
echo   --help, -h         Show this help message
echo.
echo Components:
echo   dag     - Test DAG Engine (Rust)
echo   agent   - Test Agent Chain (Go)
echo   all     - Test all components (default)
echo.
echo Examples:
echo   test.bat                      # Run all tests
echo   test.bat --component dag      # Test DAG engine only
echo   test.bat --verbose --coverage # Run all tests with coverage
exit /b 0

:start_tests
echo ========================================
echo  CREDITS ALT-LEDGER 2030 - Test Runner
echo ========================================
echo.

if /I "%COMPONENT%"=="all" goto :test_all
if /I "%COMPONENT%"=="dag" goto :test_dag
if /I "%COMPONENT%"=="agent" goto :test_agent
echo ERROR: Unknown component: %COMPONENT%
echo Use --help for available options
exit /b 1

:test_all
call :test_dag
if errorlevel 1 exit /b 1
call :test_agent
if errorlevel 1 exit /b 1
echo.
echo ✅ All tests passed!
exit /b 0

:test_dag
echo Testing DAG Engine (Rust)...
echo =============================
cd /d "%~dp0..\core"

if "%VERBOSE%"=="1" (
    set "TEST_FLAGS=-v"
) else (
    set "TEST_FLAGS="
)

if "%COVERAGE%"=="1" (
    echo Running with coverage...
    cargo tarpaulin %TEST_FLAGS% --out Html --output-dir coverage
    if errorlevel 1 (
        echo ❌ DAG tests failed!
        exit /b 1
    )
    echo Coverage report generated: core\coverage\tarpaulin-report.html
) else (
    cargo test %TEST_FLAGS%
    if errorlevel 1 (
        echo ❌ DAG tests failed!
        exit /b 1
    )
)

echo ✅ DAG tests passed!
cd /d "%~dp0"
exit /b 0

:test_agent
echo.
echo Testing Agent Chain (Go)...
echo ============================
cd /d "%~dp0..\agent\go"

if "%VERBOSE%"=="1" (
    set "GO_TEST_FLAGS=-v"
) else (
    set "GO_TEST_FLAGS="
)

if "%COVERAGE%"=="1" (
    go test %GO_TEST_FLAGS% -coverprofile=coverage.out ./...
    if errorlevel 1 (
        echo ❌ Agent tests failed!
        exit /b 1
    )
    go tool cover -html=coverage.out -o coverage.html
    echo Coverage report generated: agent\go\coverage.html
) else (
    go test %GO_TEST_FLAGS% ./...
    if errorlevel 1 (
        echo ❌ Agent tests failed!
        exit /b 1
    )
)

echo ✅ Agent tests passed!
cd /d "%~dp0"
exit /b 0