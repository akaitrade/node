@echo off
REM CREDITS ALT-LEDGER 2030 - Node 2 (Peer) Starter

echo ========================================
echo  Node 2 - Peer Node
echo ========================================
echo.

REM Create necessary directories
if not exist "data_node2" mkdir data_node2
if not exist "logs" mkdir logs

echo Node 2 Configuration:
echo - Role: Peer Node
echo - Port: 8082 (RPC: 8083)
echo - Validator ID: validator_2
echo - Connects to: localhost:8080 (Node 1)
echo.

echo Starting peer node...
echo Make sure Node 1 is running first!
echo.

REM Start the node - path from node2 to scripts is ..\..\..\scripts
..\..\..\scripts\run_node.bat --data-dir ./data_node2 --port 8082 --rpc-port 8083 --validator-id validator_2 --bootstrap-peer 127.0.0.1:8080

pause