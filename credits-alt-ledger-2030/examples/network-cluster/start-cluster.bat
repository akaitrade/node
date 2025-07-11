@echo off
REM CREDITS ALT-LEDGER 2030 - Four Node Cluster Starter
REM Full blockchain network with 4 validators

echo ============================================
echo  CREDITS ALT-LEDGER 2030 - Network Cluster
echo ============================================
echo.

REM Create directories for all nodes
echo Setting up cluster directories...
for /L %%i in (1,1,4) do (
    mkdir node%%i\data_node%%i 2>nul
    mkdir node%%i\logs 2>nul
    copy /Y genesis.json node%%i\ >nul
)

echo Cluster Configuration:
echo - Node 1 (Bootstrap): localhost:8080 (RPC: 8081)
echo - Node 2 (Validator): localhost:8082 (RPC: 8083)  
echo - Node 3 (Validator): localhost:8084 (RPC: 8085)
echo - Node 4 (Validator): localhost:8086 (RPC: 8087)
echo - Consensus: 4-node BFT with 67%% threshold (3 of 4 nodes)
echo - Sharding: 4 initial shards for load distribution
echo.

REM Find scripts directory - from network-cluster to root/scripts
set "SCRIPTS_DIR=..\..\scripts"
if not exist "%SCRIPTS_DIR%\run_node.bat" (
    echo ERROR: Cannot find scripts directory at %SCRIPTS_DIR%
    echo Current directory: %CD%
    pause
    exit /b 1
)

echo Starting 4-node cluster...
echo.

REM Generate node configs on the fly
call :generate_configs

echo Starting Node 1 (Bootstrap)...
start "Node 1 - Bootstrap" cmd /k "cd node1 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node1 --port 8080 --rpc-port 8081 --validator-id validator_1"

echo Waiting for bootstrap node...
timeout /t 3 /nobreak >nul

echo Starting Node 2 (Validator)...
start "Node 2 - Validator" cmd /k "cd node2 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node2 --port 8082 --rpc-port 8083 --validator-id validator_2"

timeout /t 2 /nobreak >nul

echo Starting Node 3 (Validator)...
start "Node 3 - Validator" cmd /k "cd node3 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node3 --port 8084 --rpc-port 8085 --validator-id validator_3"

timeout /t 2 /nobreak >nul

echo Starting Node 4 (Validator)...
start "Node 4 - Validator" cmd /k "cd node4 && ..\..\..\scripts\run_node.bat --config config.toml --data-dir ./data_node4 --port 8086 --rpc-port 8087 --validator-id validator_4"

echo.
echo ============================================
echo  4-Node Cluster Started!
echo ============================================
echo.
echo Network Status URLs:
echo - Node 1: http://localhost:8081/status
echo - Node 2: http://localhost:8083/status
echo - Node 3: http://localhost:8085/status  
echo - Node 4: http://localhost:8087/status
echo.
echo Cluster Testing:
echo   1. Wait 15-20 seconds for full network formation
echo   2. Check all nodes show 3 peers connected
echo   3. Create transactions on any node
echo   4. Verify synchronization across all nodes
echo.
echo Load Testing:
echo   curl -X POST http://localhost:8081/create -d "{\"data\":\"test tx\"}"
echo.
echo To stop cluster:
echo   Close all 4 node windows
echo.
pause
goto :end

:generate_configs
REM Generate config for each node
for /L %%i in (1,1,4) do (
    set /a PORT=8078+%%i*2
    set /a RPC_PORT=8079+%%i*2
    
    (
    echo # CREDITS ALT-LEDGER 2030 - Node %%i Configuration
    echo # Generated cluster configuration
    echo.
    echo [node]
    echo validator_id = "validator_%%i"
    echo node_name = "cluster-node-%%i"
    echo listen_port = !PORT!
    echo rpc_port = !RPC_PORT!
    echo enable_rpc = true
    echo phase = 3
    echo data_dir = "./data_node%%i"
    echo log_level = "info"
    echo.
    echo [dag_engine]
    echo storage_backend = "sled"
    echo storage_path = "dag_data"
    echo cache_size = 15000
    echo max_vertices_per_shard = 1000000
    echo min_vertices_per_shard = 100000
    echo.
    echo [consensus]
    echo voting_threshold = 0.67
    echo max_validators = 4
    echo round_timeout_seconds = 30
    echo finality_depth = 10
    echo.
    echo [sharding]
    echo initial_shard_count = 4
    echo rebalance_threshold = 0.8
    echo max_shard_size = 1000000
    echo min_shard_size = 100000
    echo.
    echo [networking]
    echo transport_protocol = "ctdp_v2"
    echo enable_quic = true
    echo max_connections = 100
    echo connection_timeout_seconds = 30
    if %%i GTR 1 (
        echo peers = ["localhost:8080"]
    ) else (
        echo peers = []
    )
    echo enable_discovery = true
    echo enable_local_discovery = true
    echo bind_address = "0.0.0.0"
    echo.
    echo [performance]
    echo worker_threads = 4
    echo max_memory_usage_mb = 2048
    echo gc_interval_seconds = 60
    echo.
    echo [security]
    echo enable_tls = false
    echo enable_rate_limiting = true
    echo max_requests_per_second = 500
    echo.
    echo [logging]
    echo log_file = "./logs/node%%i.log"
    echo max_log_size_mb = 100
    echo log_rotation_count = 5
    echo enable_metrics = true
    echo.
    echo [development]
    echo enable_debug_api = true
    echo simulate_network_delay = false
    echo enable_profiling = false
    echo test_mode = false
    ) > node%%i\config.toml
)
exit /b 0

:end