@echo off
REM CREDITS ALT-LEDGER 2030 - Node 1 (Bootstrap) Starter

echo ========================================
echo  Node 1 - Bootstrap Node
echo ========================================
echo.

REM Create necessary directories
if not exist "data_node1" mkdir data_node1
if not exist "logs" mkdir logs

echo Node 1 Configuration:
echo - Role: Bootstrap Node
echo - Port: 8080 (RPC: 8081)
echo - Validator ID: validator_1
echo - Peers: None (accepts connections)
echo.

echo Starting bootstrap node...
echo Other nodes can connect to: localhost:8080
echo.

REM Start the node - path from node1 to scripts is ..\..\..\scripts  
..\..\..\scripts\run_node.bat --data-dir ./data_node1 --port 8080 --rpc-port 8081 --validator-id validator_1

pause