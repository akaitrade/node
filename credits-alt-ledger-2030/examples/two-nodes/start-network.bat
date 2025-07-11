@echo off
REM CREDITS ALT-LEDGER 2030 - Two Node Network Starter
REM Start both nodes with proper configuration

echo ==========================================
echo  CREDITS ALT-LEDGER 2030 - Two Node Network
echo ==========================================
echo.

REM Create directories for both nodes
echo Setting up node directories...
mkdir node1\data_node1 2>nul
mkdir node1\logs 2>nul
mkdir node2\data_node2 2>nul  
mkdir node2\logs 2>nul

REM Copy bootstrap config to both nodes
copy /Y bootstrap.json node1\ >nul
copy /Y bootstrap.json node2\ >nul

echo Network Configuration:
echo - Node 1 (Bootstrap): localhost:8080 (RPC: 8081)
echo - Node 2 (Peer):      localhost:8082 (RPC: 8083)
echo - Consensus: 2-node network with 67%% threshold
echo - Protocol: CTDP v2 with QUIC transport
echo.

REM Find scripts directory - from two-nodes folder, go up to root, then to scripts
set "SCRIPTS_DIR=..\..\scripts"
if not exist "%SCRIPTS_DIR%\run_node.bat" (
    echo ERROR: Cannot find scripts directory at %SCRIPTS_DIR%
    echo Current directory: %CD%
    echo Looking for: %SCRIPTS_DIR%\run_node.bat
    pause
    exit /b 1
)

echo Starting Node 1 (Bootstrap)...
start "Node 1 - Bootstrap" cmd /k "cd node1 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node1 --port 8080 --rpc-port 8081 --validator-id validator_1"

echo Waiting for Node 1 to initialize...
timeout /t 5 /nobreak >nul

echo Starting Node 2 (Peer)...  
start "Node 2 - Peer" cmd /k "cd node2 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node2 --port 8082 --rpc-port 8083 --validator-id validator_2"

echo.
echo ==========================================
echo  Two-Node Network Started!
echo ==========================================
echo.
echo Network Status:
echo - Bootstrap Node: http://localhost:8081/status
echo - Peer Node:      http://localhost:8083/status
echo.
echo To test the network:
echo   1. Wait 10-15 seconds for nodes to connect
echo   2. Open either node CLI window
echo   3. Try commands: status, stats, create "test"
echo.
echo To stop the network:
echo   Close both node windows or press Ctrl+C in each
echo.
echo Log files:
echo   node1\logs\node1.log
echo   node2\logs\node2.log
echo.
pause