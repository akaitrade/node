@echo off
REM
REM CREDITS ALT-LEDGER 2030 - Node Runner Script (Windows)
REM
REM Convenience script to run the standalone blockchain node
REM with various configuration options
REM

setlocal enabledelayedexpansion

REM Colors for output (Windows compatible)
set "INFO=[INFO]"
set "SUCCESS=[SUCCESS]"
set "WARNING=[WARNING]"
set "ERROR=[ERROR]"

REM Project root
set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "CORE_DIR=%PROJECT_ROOT%\core"
set "AGENT_DIR=%PROJECT_ROOT%\agent\go"

REM Default configuration
set "NODE_TYPE=dag"
set "DATA_DIR=%PROJECT_ROOT%\data"
set "PHASE=3"
set "PORT=8080"
set "RPC_PORT=8081"
set "VALIDATOR_ID=validator_1"
set "AGENT_NAME=agent_1"
set "CONFIG_FILE=%PROJECT_ROOT%\config.toml"

REM Parse command line arguments
:parse_args
if "%~1"=="" goto :args_done
if "%~1"=="--type" (
    set "NODE_TYPE=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--phase" (
    set "PHASE=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--port" (
    set "PORT=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--rpc-port" (
    set "RPC_PORT=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--data-dir" (
    set "DATA_DIR=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--validator-id" (
    set "VALIDATOR_ID=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--agent-name" (
    set "AGENT_NAME=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--config" (
    set "CONFIG_FILE=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--peer" (
    set "PEER=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--bootstrap" (
    set "BOOTSTRAP=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="--help" (
    echo CREDITS ALT-LEDGER 2030 - Node Runner
    echo.
    echo Usage: %~nx0 [OPTIONS]
    echo.
    echo Options:
    echo   --type ^<dag^|agent^>       Node type (default: dag)
    echo   --phase ^<1^|2^|3^>          Migration phase (default: 3)
    echo   --port ^<port^>            Listen port (default: 8080)
    echo   --rpc-port ^<port^>        RPC port (default: 8081)
    echo   --data-dir ^<path^>        Data directory (default: ./data)
    echo   --validator-id ^<id^>      Validator ID (default: validator_1)
    echo   --agent-name ^<name^>      Agent name (default: agent_1)
    echo   --config ^<file^>          Configuration file (default: config.toml)
    echo   --peer ^<address^>         Connect to specific peer (e.g., 192.168.1.100:8080)
    echo   --bootstrap ^<address^>    Bootstrap node address
    echo   --help                   Show this help message
    echo.
    echo Node Types:
    echo   dag     - DAG engine blockchain node (Rust)
    echo   agent   - Agent chain personal blockchain (Go)
    echo.
    echo Examples:
    echo   %~nx0                              # Run DAG node with defaults
    echo   %~nx0 --type agent                 # Run agent chain
    echo   %~nx0 --phase 1 --port 9000        # Run Phase 1 DAG node on port 9000
    echo   %~nx0 --peer 192.168.1.100:8080    # Connect to specific peer
    echo   %~nx0 --bootstrap localhost:8080   # Connect to bootstrap node
    echo   %~nx0 --type agent --agent-name alice # Run agent chain for 'alice'
    goto :end
)
echo %ERROR% Unknown option: %~1
goto :end

:args_done

REM Create necessary directories
if not exist "%DATA_DIR%" mkdir "%DATA_DIR%"
if not exist "%DATA_DIR%\logs" mkdir "%DATA_DIR%\logs"
if not exist "%DATA_DIR%\certs" mkdir "%DATA_DIR%\certs"

REM Show node info
echo %INFO% CREDITS ALT-LEDGER 2030 - Node Information
echo ================================
echo Node Type: %NODE_TYPE%
echo Phase: %PHASE%
echo Data Directory: %DATA_DIR%
echo Configuration: %CONFIG_FILE%
echo Project Root: %PROJECT_ROOT%
echo Build Directory: %BUILD_DIR%
echo ================================

REM Set environment variables
set "CREDITS_DATA_DIR=%DATA_DIR%"
set "CREDITS_PHASE=%PHASE%"
set "CREDITS_LOG_LEVEL=info"
set "RUST_LOG=info"

REM Check node type and run appropriate node
if "%NODE_TYPE%"=="dag" (
    echo %INFO% Starting DAG Engine Node
    echo %INFO% Phase: %PHASE%
    echo %INFO% Port: %PORT%
    echo %INFO% RPC Port: %RPC_PORT%
    echo %INFO% Data Directory: %DATA_DIR%
    echo %INFO% Validator ID: %VALIDATOR_ID%
    
    cd /d "%CORE_DIR%"
    
    REM Check if we can use cargo
    cargo --version >nul 2>&1
    if !errorlevel! == 0 (
        echo %INFO% Running with Cargo...
        cargo run --release --bin credits-node -- --data-dir "%DATA_DIR%" --port %PORT% --rpc-port %RPC_PORT% --phase %PHASE% --validator-id "%VALIDATOR_ID%"
    ) else (
        echo %WARNING% Cargo not found. Checking for pre-built executable...
        
        REM Look for pre-built executable
        set "EXECUTABLE="
        if exist "%BUILD_DIR%\Release\credits-node.exe" (
            set "EXECUTABLE=%BUILD_DIR%\Release\credits-node.exe"
        ) else if exist "%CORE_DIR%\target\release\credits-node.exe" (
            set "EXECUTABLE=%CORE_DIR%\target\release\credits-node.exe"
        )
        
        if defined EXECUTABLE (
            echo %INFO% Using pre-built executable: !EXECUTABLE!
            "!EXECUTABLE!" --data-dir "%DATA_DIR%" --port %PORT% --rpc-port %RPC_PORT% --phase %PHASE% --validator-id "%VALIDATOR_ID%"
        ) else (
            echo %ERROR% No executable found. Please build the project first.
            goto :end
        )
    )
) else if "%NODE_TYPE%"=="agent" (
    echo %INFO% Starting Agent Chain Node
    echo %INFO% Agent Name: %AGENT_NAME%
    echo %INFO% Data Directory: %DATA_DIR%
    
    cd /d "%AGENT_DIR%"
    
    REM Check if we can use go
    go version >nul 2>&1
    if !errorlevel! == 0 (
        echo %INFO% Running with Go...
        go run main.go agent_chain.go cross_agent.go --data-dir "%DATA_DIR%" --agent-name "%AGENT_NAME%"
    ) else (
        echo %WARNING% Go not found. Checking for pre-built executable...
        
        REM Look for pre-built executable
        set "EXECUTABLE="
        if exist "%BUILD_DIR%\agent-cli.exe" (
            set "EXECUTABLE=%BUILD_DIR%\agent-cli.exe"
        ) else if exist "%AGENT_DIR%\agent-cli.exe" (
            set "EXECUTABLE=%AGENT_DIR%\agent-cli.exe"
        )
        
        if defined EXECUTABLE (
            echo %INFO% Using pre-built executable: !EXECUTABLE!
            "!EXECUTABLE!" --data-dir "%DATA_DIR%" --agent-name "%AGENT_NAME%"
        ) else (
            echo %ERROR% No executable found. Please build the project first.
            goto :end
        )
    )
) else (
    echo %ERROR% Invalid node type: %NODE_TYPE%
    echo %ERROR% Supported types: dag, agent
    goto :end
)

:end
pause