@echo off
REM CREDITS ALT-LEDGER 2030 - Clean Single Node Data

echo ========================================
echo  Single Node - Data Cleanup
echo ========================================
echo.

echo This will delete all blockchain data and logs.
echo Press Ctrl+C to cancel, or
pause

echo.
echo Cleaning node data...
if exist "data_single" rmdir /s /q "data_single"
if exist "logs" rmdir /s /q "logs"

echo Recreating directories...
mkdir data_single 2>nul
mkdir logs 2>nul

echo.
echo ✅ Single node data cleanup complete!
echo You can start the node with: start.bat
echo.
pause