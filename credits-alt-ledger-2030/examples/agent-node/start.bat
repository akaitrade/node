@echo off
REM CREDITS ALT-LEDGER 2030 - Agent Node Starter
REM Personal blockchain agent with DAG synchronization

echo ========================================
echo  CREDITS ALT-LEDGER 2030 - Agent Node
echo ========================================
echo.

REM Create necessary directories
if not exist "data_agent" mkdir data_agent
if not exist "logs" mkdir logs
if not exist "keys" mkdir keys

echo Starting personal agent node...
echo.
echo Agent Configuration:
echo - Agent Name: alice
echo - Type: Personal Agent Chain
echo - Port: 8090 (RPC: 8091, App API: 8092)
echo - DAG Sync: Enabled (connects to localhost:8080)
echo - Personal Chain: alice_chain_001
echo - Offline Mode: Supported
echo.

REM Find the correct path to scripts
set "SCRIPTS_DIR=..\..\scripts"
if not exist "%SCRIPTS_DIR%\run_node.bat" (
    set "SCRIPTS_DIR=..\..\..\scripts"
)
if not exist "%SCRIPTS_DIR%\run_node.bat" (
    echo ERROR: Cannot find scripts directory
    echo Please ensure you're running from the examples\agent-node directory
    pause
    exit /b 1
)

echo Agent Features:
echo - ✅ Personal blockchain
echo - ✅ DAG network synchronization  
echo - ✅ Cross-agent communication
echo - ✅ Smart contract support
echo - ✅ Built-in wallet, identity, messaging
echo - ✅ Offline operation capability
echo.

echo Starting agent...
echo Press Ctrl+C to stop the agent
echo.

REM Start the agent node
"%SCRIPTS_DIR%\run_node.bat" --type agent --config config.toml --data-dir ./data_agent --port 8090 --rpc-port 8091 --agent-name alice

echo.
echo Agent stopped.
pause