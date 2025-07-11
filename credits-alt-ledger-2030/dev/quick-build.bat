@echo off
REM Quick incremental build for development
REM Fast builds without full optimization

echo Quick Build - Development Mode
echo ==============================
echo.

REM Build DAG engine (debug mode for speed)
echo Building DAG Engine...
cd /d "%~dp0..\core"
cargo build --bin credits-node
if errorlevel 1 (
    echo ❌ DAG build failed!
    pause
    exit /b 1
)
echo ✅ DAG Engine built

echo.
echo Building Agent Chain...
cd /d "%~dp0..\agent\go"
go build -o agent.exe .
if errorlevel 1 (
    echo ❌ Agent build failed!
    pause
    exit /b 1
)
echo ✅ Agent Chain built

echo.
echo Quick build complete!
echo.
echo Executables:
echo   DAG Node: core\target\debug\credits-node.exe
echo   Agent:    agent\go\agent.exe
echo.
cd /d "%~dp0"