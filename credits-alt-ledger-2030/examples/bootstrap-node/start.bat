@echo off
REM CREDITS ALT-LEDGER 2030 - Bootstrap Node Starter
REM Production-ready bootstrap node

echo ========================================
echo  Bootstrap Node - Production Ready
echo ========================================
echo.

REM Create necessary directories
if not exist "data_bootstrap" mkdir data_bootstrap
if not exist "logs" mkdir logs
if not exist "certs" mkdir certs
if not exist "backups" mkdir backups

echo Bootstrap Node Configuration:
echo - Role: Production Bootstrap Node
echo - Port: 8080 (RPC: 8081)
echo - Health Check: 8090
echo - Metrics: 8091
echo - Security: TLS Enabled
echo - Monitoring: Prometheus Enabled
echo.

echo Bootstrap Features:
echo - ✅ High performance (8GB RAM, 8 CPU cores)
echo - ✅ Security (TLS, rate limiting, IP whitelist)
echo - ✅ Monitoring (health checks, metrics, logging)
echo - ✅ Backup (auto-backup every 6 hours)
echo - ✅ Production database (RocksDB)
echo.

echo Starting bootstrap node...
echo Other nodes can connect to this bootstrap at: <your-ip>:8080
echo.

REM Start the node - path from bootstrap-node to scripts is ..\..\scripts
..\..\scripts\run_node.bat --config config.toml --data-dir ./data_bootstrap --port 8080 --rpc-port 8081 --validator-id bootstrap_node_main

pause