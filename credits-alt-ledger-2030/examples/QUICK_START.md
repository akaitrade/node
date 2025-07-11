# CREDITS ALT-LEDGER 2030 - Quick Start Guide

Get up and running with CREDITS blockchain in minutes!

## 🚀 Fastest Start (1 minute)

```bash
# 1. Copy single node example
xcopy /E examples\single-node my-first-node\

# 2. Start the node
cd my-first-node
start.bat

# 3. Use the CLI
# In the node window, try: help, status, create "hello world"
```

## 🔗 Test Networking (2 minutes)

```bash
# Start a 2-node network
cd examples\two-nodes
start-network.bat

# Test in either node CLI:
# create "test transaction"
# stats
```

## 🌐 Full Network (3 minutes)

```bash
# Start 4-node cluster
cd examples\network-cluster
start-cluster.bat

# Load test:
# curl -X POST http://localhost:8081/create -d "{\"data\":\"load test\"}"
```

## 🤖 Personal Agent (1 minute)

```bash
# Start personal blockchain
cd examples\agent-node
start.bat

# Features: wallet, identity, messaging, file storage
```

## ⚡ Super Quick Commands

| Want to... | Command |
|------------|---------|
| **Learn basics** | `cd examples\single-node && start.bat` |
| **Test networking** | `cd examples\two-nodes && start-network.bat` |
| **Full blockchain** | `cd examples\network-cluster && start-cluster.bat` |
| **Personal chain** | `cd examples\agent-node && start.bat` |
| **Production setup** | `cd examples\bootstrap-node && start.bat` |

## 📋 What Each Example Gives You

### single-node/ ⭐
- **Time**: 30 seconds to start
- **Use**: Learning, development
- **Features**: Full blockchain, CLI, API
- **Perfect for**: First time users

### two-nodes/ ⭐⭐
- **Time**: 1 minute to start
- **Use**: Network testing
- **Features**: Peer connection, consensus
- **Perfect for**: Understanding networking

### network-cluster/ ⭐⭐⭐
- **Time**: 2 minutes to start
- **Use**: Production simulation
- **Features**: 4-node BFT, sharding, load balancing
- **Perfect for**: Testing applications

### bootstrap-node/ ⭐⭐⭐⭐
- **Time**: 5 minutes to configure
- **Use**: Production deployment
- **Features**: Security, monitoring, backups
- **Perfect for**: Real networks

### agent-node/ ⭐⭐
- **Time**: 1 minute to start
- **Use**: Personal blockchain
- **Features**: Offline mode, apps, sync
- **Perfect for**: Personal use cases

## 🛠️ Prerequisites Check

```bash
# Check if you have everything needed
dev\check-deps.bat

# Auto-install missing tools
dev\check-deps.bat --install
```

## 📖 Next Steps

1. **Start with single-node** - Get familiar with the CLI
2. **Try two-nodes** - Learn about networking
3. **Run network-cluster** - Experience full consensus
4. **Build your app** - Use the HTTP API
5. **Deploy production** - Use bootstrap-node template

## 🔧 Common Tasks

### Build Everything
```bash
scripts\build.bat --phase 3
```

### Run Tests
```bash
scripts\test.bat
```

### Debug Issues
```bash
scripts\debug.bat
```

### Clean Start
```bash
dev\clean-build.bat
```

## 🆘 Need Help?

- **Examples not working?** Check `scripts\debug.bat`
- **Build failing?** Run `dev\check-deps.bat --install`
- **Database lock errors?** Run `clean-data.bat` in the example directory
- **Ports taken?** Edit `config.toml` files in examples
- **More help?** Read the detailed README.md in each example

## 🔧 Database Conflicts (All Fixed!)

All examples now use isolated data directories:
- **single-node** → `data_single/`
- **two-nodes** → `data_node1/`, `data_node2/`
- **network-cluster** → `data_node1/` through `data_node4/`
- **bootstrap-node** → `data_bootstrap/`
- **agent-node** → `data_agent/`

No more database lock conflicts! 🎉

## 🎯 Choose Your Path

| I want to... | Start here |
|--------------|------------|
| Learn blockchain basics | `examples\single-node\` |
| Build a dApp | `examples\two-nodes\` |
| Test performance | `examples\network-cluster\` |
| Deploy production | `examples\bootstrap-node\` |
| Use personal blockchain | `examples\agent-node\` |

Happy blockchain building! 🚀