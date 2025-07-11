@echo off
REM CREDITS ALT-LEDGER 2030 - Clean Agent Data

echo ========================================
echo  Agent Node - Data Cleanup
echo ========================================
echo.

echo This will delete all agent data, logs, and keys.
echo Press Ctrl+C to cancel, or
pause

echo.
echo Cleaning agent data...
if exist "data_agent" rmdir /s /q "data_agent"
if exist "logs" rmdir /s /q "logs"
if exist "keys" rmdir /s /q "keys"

echo Recreating directories...
mkdir data_agent 2>nul
mkdir logs 2>nul
mkdir keys 2>nul

echo.
echo ✅ Agent data cleanup complete!
echo You can start the agent with: start.bat
echo.
pause