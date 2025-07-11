@echo off
REM CREDITS ALT-LEDGER 2030 - Clean Cluster Data
REM Remove all data files for fresh start

echo ========================================
echo  Network Cluster - Data Cleanup
echo ========================================
echo.

echo This will delete all blockchain data and logs for 4 nodes.
echo Press Ctrl+C to cancel, or
pause

echo.
echo Cleaning all node data...
for /L %%i in (1,1,4) do (
    echo Cleaning Node %%i...
    if exist "node%%i\data_node%%i" rmdir /s /q "node%%i\data_node%%i"
    if exist "node%%i\logs" rmdir /s /q "node%%i\logs"
    if exist "node%%i\config.toml" del /f "node%%i\config.toml"
)

echo.
echo Recreating directories...
for /L %%i in (1,1,4) do (
    mkdir node%%i\data_node%%i 2>nul
    mkdir node%%i\logs 2>nul
)

echo.
echo ✅ Cluster data cleanup complete!
echo All 4 nodes now have fresh data directories.
echo You can start the cluster with: start-cluster.bat
echo.
pause