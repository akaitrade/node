@echo off
REM CREDITS ALT-LEDGER 2030 - Clean Two-Node Data
REM Remove all data files for fresh start

echo ========================================
echo  Two-Node Network - Data Cleanup
echo ========================================
echo.

echo This will delete all blockchain data and logs for both nodes.
echo Press Ctrl+C to cancel, or
pause

echo.
echo Cleaning Node 1 data...
if exist "node1\data_node1" rmdir /s /q "node1\data_node1"
if exist "node1\logs" rmdir /s /q "node1\logs"

echo Cleaning Node 2 data...
if exist "node2\data_node2" rmdir /s /q "node2\data_node2"
if exist "node2\logs" rmdir /s /q "node2\logs"

echo.
echo Recreating directories...
mkdir node1\data_node1 2>nul
mkdir node1\logs 2>nul
mkdir node2\data_node2 2>nul
mkdir node2\logs 2>nul

echo.
echo ✅ Data cleanup complete!
echo Both nodes now have fresh data directories.
echo You can start the network with: start-network.bat
echo.
pause