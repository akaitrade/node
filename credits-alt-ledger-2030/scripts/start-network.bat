@echo off
REM CREDITS ALT-LEDGER 2030 - Network Starter
REM Easily start multi-node networks for testing

setlocal enabledelayedexpansion

REM Default settings
set "NODE_COUNT=2"
set "BASE_PORT=8080"
set "BASE_RPC_PORT=8081"
set "NETWORK_TYPE=local"

REM Parse command line arguments
:parse_args
if "%~1"=="" goto :start_network
if /I "%~1"=="--help" goto :show_help
if /I "%~1"=="-h" goto :show_help
if /I "%~1"=="--nodes" (
    set "NODE_COUNT=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--base-port" (
    set "BASE_PORT=%~2"
    shift
    shift
    goto :parse_args
)
if /I "%~1"=="--type" (
    set "NETWORK_TYPE=%~2"
    shift
    shift
    goto :parse_args
)
shift
goto :parse_args

:show_help
echo CREDITS ALT-LEDGER 2030 - Network Starter
echo.
echo Usage: start-network.bat [OPTIONS]
echo.
echo Options:
echo   --nodes ^<count^>     Number of nodes to start (default: 2)
echo   --base-port ^<port^>  Starting port number (default: 8080)
echo   --type ^<type^>       Network type: local, testnet (default: local)
echo   --help, -h          Show this help message
echo.
echo Examples:
echo   start-network.bat                    # Start 2 nodes
echo   start-network.bat --nodes 4          # Start 4 nodes
echo   start-network.bat --nodes 3 --base-port 9000
echo.
echo The script will:
echo   - Create separate directories for each node
echo   - Configure nodes to connect to each other
echo   - Start nodes in separate windows
echo   - Create a bootstrap.json for easy connections
exit /b 0

:start_network
echo ==========================================
echo  CREDITS ALT-LEDGER 2030 - Network Starter
echo ==========================================
echo.
echo Starting %NODE_COUNT% nodes...
echo Base port: %BASE_PORT%
echo Network type: %NETWORK_TYPE%
echo.

REM Create network directory
set "NETWORK_DIR=test-network-%DATE:~-4%%DATE:~4,2%%DATE:~7,2%-%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%"
set "NETWORK_DIR=%NETWORK_DIR: =0%"
mkdir "%NETWORK_DIR%" 2>nul
cd "%NETWORK_DIR%"

REM Create bootstrap configuration
echo Creating bootstrap configuration...
(
echo {
echo   "network_id": "%NETWORK_TYPE%_%NETWORK_DIR%",
echo   "bootstrap_nodes": [
echo     {
echo       "id": "validator_1",
echo       "address": "localhost:%BASE_PORT%",
echo       "public_key": "genesis_key_placeholder"
echo     }
echo   ]
echo }
) > bootstrap.json

REM Start nodes
for /L %%i in (1,1,%NODE_COUNT%) do (
    set /a PORT=%BASE_PORT%+%%i-1
    set /a RPC_PORT=%BASE_RPC_PORT%+%%i-1
    
    echo.
    echo Starting Node %%i...
    echo   - Port: !PORT!
    echo   - RPC Port: !RPC_PORT!
    echo   - Data Dir: node%%i
    
    REM Create node directory
    mkdir node%%i 2>nul
    
    REM Copy bootstrap.json to node directory
    copy /Y bootstrap.json node%%i\ >nul
    
    REM Create node-specific config
    (
    echo [node]
    echo validator_id = "validator_%%i"
    echo node_name = "test-node-%%i"
    echo listen_port = !PORT!
    echo rpc_port = !RPC_PORT!
    echo data_dir = "./data"
    echo.
    echo [networking]
    echo listen_port = !PORT!
    if %%i GTR 1 (
        echo # Connect to previous nodes
        echo peers = [
        for /L %%j in (1,1,%%i-1) do (
            set /a PEER_PORT=%BASE_PORT%+%%j-1
            if %%j LSS %%i-1 (
                echo     "localhost:!PEER_PORT!",
            ) else (
                echo     "localhost:!PEER_PORT!"
            )
        )
        echo ]
    )
    echo.
    echo [development]
    echo test_mode = true
    ) > node%%i\config.toml
    
    REM Start node in new window
    if %%i EQU 1 (
        REM First node is the genesis/bootstrap node
        start "Node %%i - Bootstrap" cmd /k "cd node%%i && ..\..\..\..\scripts\run_node.bat --config config.toml"
    ) else (
        REM Other nodes connect to bootstrap
        start "Node %%i" cmd /k "cd node%%i && ..\..\..\..\scripts\run_node.bat --config config.toml"
    )
    
    REM Wait a bit before starting next node
    timeout /t 2 /nobreak >nul
)

echo.
echo ==========================================
echo  Network Started Successfully!
echo ==========================================
echo.
echo Network Directory: %CD%
echo Number of Nodes: %NODE_COUNT%
echo Port Range: %BASE_PORT% - !PORT!
echo.
echo To connect another node to this network:
echo   1. Copy bootstrap.json to the new node's directory
echo   2. Run: scripts\run_node.bat --peer localhost:%BASE_PORT%
echo.
echo To stop all nodes:
echo   Close all Node windows or press Ctrl+C in each
echo.
echo Node Directories:
for /L %%i in (1,1,%NODE_COUNT%) do (
    echo   - node%%i\
)
echo.
pause