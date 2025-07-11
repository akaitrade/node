@echo off
REM CREDITS ALT-LEDGER 2030 - Single Node Starter
REM Simple development node for learning and testing

echo ========================================
echo  CREDITS ALT-LEDGER 2030 - Single Node
echo ========================================
echo.

REM Create necessary directories
if not exist "data_single" mkdir data_single
if not exist "logs" mkdir logs

echo Starting single development node...
echo.
echo Node Configuration:
echo - Type: DAG Blockchain Node
echo - Mode: Development (standalone)
echo - Port: 8080
echo - RPC Port: 8081
echo - Data Dir: ./data
echo - Config: ./config.toml
echo.

REM Find the correct path to scripts - from single-node to root/scripts
set "SCRIPTS_DIR=..\..\scripts"
if not exist "%SCRIPTS_DIR%\run_node.bat" (
    echo ERROR: Cannot find scripts directory at %SCRIPTS_DIR%
    echo Current directory: %CD%
    echo Please ensure you're running from the examples\single-node directory
    pause
    exit /b 1
)

echo Starting node...
echo Press Ctrl+C to stop the node
echo.

REM Start the node with this configuration
"%SCRIPTS_DIR%\run_node.bat" --config config.toml --data-dir ./data_single --port 8080 --rpc-port 8081 --validator-id dev-node-1

echo.
echo Node stopped.
pause