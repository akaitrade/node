@echo off
REM Debug library compilation issues

echo Debugging Library Compilation...
cd /d "%~dp0"
cd core

echo Checking individual modules...
echo ==============================

echo Checking dag_vertex module...
echo // Test dag_vertex > test_dag_vertex.rs
echo use crate::dag_vertex::VertexHash; >> test_dag_vertex.rs
echo fn main() { println!("test"); } >> test_dag_vertex.rs

echo Checking error module...
cargo check --lib 2>&1 | findstr "error"
if errorlevel 1 (
    echo No specific error module issues found
)

echo Full library check...
cargo check --lib
if errorlevel 1 (
    echo ERROR: Library compilation has issues
    echo This is why modules aren't available to main.rs
    pause
    exit /b 1
)

echo SUCCESS: Library compiles without errors
echo.
echo This means the issue is with how we're importing in main.rs
echo The modules exist and compile, but we're not accessing them correctly
echo.
pause