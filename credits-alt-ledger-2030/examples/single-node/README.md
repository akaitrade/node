# Single Node Example

A simple standalone CREDITS ALT-LEDGER 2030 node for development and testing.

## What This Example Provides

- ✅ **Standalone Node**: Runs independently without network connections
- ✅ **Development Mode**: Debug API enabled, detailed logging
- ✅ **Simple Configuration**: Minimal settings for easy understanding
- ✅ **Ready to Run**: All files included, just start and go

## Quick Start

### Windows
```bash
cd examples\single-node
start.bat
```

### Linux/Mac
```bash
cd examples/single-node
chmod +x start.sh
./start.sh
```

## What You'll Get

Once started, you'll have:

1. **Interactive CLI** - Command-line interface for blockchain operations
2. **RPC API** - HTTP API on port 8081 for programmatic access  
3. **Full DAG Engine** - Complete blockchain with consensus and storage
4. **Development Tools** - Debug API and profiling enabled

## Basic Usage

### CLI Commands
```bash
# In the node CLI
help                           # Show all commands
status                         # Show node status  
stats                          # Show blockchain statistics
create "hello world"           # Create a new transaction
balance addr123                # Check balance
transfer from123 to456 100    # Transfer tokens
```

### API Examples
```bash
# Check node status
curl http://localhost:8081/status

# Get blockchain stats
curl http://localhost:8081/stats

# Create transaction
curl -X POST http://localhost:8081/create -d '{"data":"hello world"}'
```

## Configuration Details

The `config.toml` file is configured for:

- **Single node operation** (no peers required)
- **Development mode** (debug API, detailed logs)
- **Embedded database** (SQLite-like Sled database)
- **Minimal resource usage** (512MB RAM, 2 threads)
- **No TLS/security** (for simplicity)

## File Structure

```
single-node/
├── config.toml       # Node configuration
├── start.bat         # Windows startup script
├── start.sh          # Linux startup script  
├── README.md         # This file
├── data/             # Blockchain data (created on first run)
│   └── dag_data/     # DAG storage
└── logs/             # Log files (created on first run)
    └── node.log      # Main log file
```

## Customization

### Change Ports
Edit `config.toml`:
```toml
[node]
listen_port = 9000    # Change from 8080
rpc_port = 9001       # Change from 8081
```

### Increase Performance  
Edit `config.toml`:
```toml
[performance]
worker_threads = 4           # More CPU threads
max_memory_usage_mb = 1024   # More RAM

[dag_engine]
cache_size = 10000          # Larger cache
```

### Enable Security
Edit `config.toml`:
```toml
[security]
enable_tls = true
enable_rate_limiting = true
max_requests_per_second = 100
```

## Next Steps

Once you're comfortable with a single node:

1. **Try two-nodes example** - Learn about node networking
2. **Run network-cluster** - Experience full consensus  
3. **Explore agent-node** - Test personal blockchain features
4. **Build your application** - Use the API to create blockchain apps

## Troubleshooting

### Node Won't Start
- Check if ports 8080/8081 are available
- Ensure you have write permissions in the directory
- Check logs/node.log for error details

### Performance Issues
- Increase memory limit in config.toml
- Add more worker threads
- Check available disk space

### API Not Working
- Verify RPC is enabled in config.toml
- Check firewall settings for port 8081
- Test with `curl http://localhost:8081/status`

### Need Help?
- Check the main README_SCRIPTS.md
- Review NODE_NETWORKING_GUIDE.md
- Examine other examples for different configurations