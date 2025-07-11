# CREDITS ALT-LEDGER 2030 - Script Documentation

## Script Organization

The scripts have been reorganized for better maintainability and consistency:

```
credits-alt-ledger-2030/
├── scripts/           # Production scripts
│   ├── build.bat     # Main build system
│   ├── run_node.bat  # Node runner
│   ├── test.bat      # Unified test runner (NEW)
│   └── debug.bat     # Debug diagnostics (NEW)
├── dev/              # Development scripts (NEW)
│   ├── quick-build.bat    # Fast incremental builds
│   ├── clean-build.bat    # Clean rebuild from scratch
│   └── check-deps.bat     # Dependency checker
└── README_SCRIPTS.md      # This file
```

## Quick Start

1. **First Time Setup:**
   ```bash
   dev\check-deps.bat --install    # Check and install dependencies
   scripts\build.bat --phase 3      # Build everything
   ```

2. **Run the Node:**
   ```bash
   scripts\run_node.bat             # Run with default settings
   scripts\run_node.bat --help      # See all options
   ```

3. **Development:**
   ```bash
   dev\quick-build.bat              # Fast debug build
   scripts\test.bat                 # Run all tests
   scripts\debug.bat                # Troubleshoot issues
   ```

## Production Scripts

### build.bat
Main build system for all components.

```bash
scripts\build.bat [OPTIONS]

Options:
  --phase <1|2|3>    Migration phase (default: 3)
  --release          Build in release mode (default)
  --debug            Build in debug mode
  --clean            Clean before building
  --help             Show help message

Examples:
  scripts\build.bat                    # Default release build
  scripts\build.bat --phase 1 --debug  # Phase 1 debug build
  scripts\build.bat --clean            # Clean rebuild
```

### run_node.bat
Runs the blockchain node with various configurations.

```bash
scripts\run_node.bat [OPTIONS]

Options:
  --type <dag|agent>    Node type (default: dag)
  --phase <1|2|3>       Migration phase (default: 3)
  --port <port>         Listen port (default: 8080)
  --data-dir <path>     Data directory
  --help                Show help message

Examples:
  scripts\run_node.bat                      # Run DAG node
  scripts\run_node.bat --type agent         # Run agent node
  scripts\run_node.bat --port 9000          # Custom port
```

### test.bat (NEW)
Unified test runner for all components.

```bash
scripts\test.bat [OPTIONS]

Options:
  --component <name>  Test specific component (dag, agent, all)
  --verbose          Enable verbose test output
  --coverage         Generate coverage reports
  --help             Show help message

Examples:
  scripts\test.bat                      # Run all tests
  scripts\test.bat --component dag      # Test DAG engine only
  scripts\test.bat --verbose --coverage # Full test with coverage
```

### debug.bat (NEW)
Comprehensive debugging and troubleshooting tool.

```bash
scripts\debug.bat [OPTIONS]

Options:
  --action <name>    Run specific debug action
  --verbose          Enable verbose output
  --help             Show help message

Actions:
  imports   - Debug import issues
  build     - Debug build problems
  libs      - Check library compilation
  deps      - Verify dependencies
  env       - Check environment setup
  all       - Run all diagnostics (default)

Examples:
  scripts\debug.bat                    # Run all diagnostics
  scripts\debug.bat --action imports   # Debug import issues
  scripts\debug.bat --verbose          # Verbose diagnostics
```

## Development Scripts

### quick-build.bat
Fast incremental build for development (debug mode).

```bash
dev\quick-build.bat

# Builds both DAG engine and Agent in debug mode
# Much faster than release builds
# Output: core\target\debug\credits-node.exe
```

### clean-build.bat
Complete clean rebuild from scratch.

```bash
dev\clean-build.bat

# WARNING: Deletes all build artifacts
# Performs full clean and rebuild
# Uses release mode by default
```

### check-deps.bat
Dependency checker with optional auto-install.

```bash
dev\check-deps.bat [OPTIONS]

Options:
  --install    Automatically install missing dependencies
  --help       Show help message

Examples:
  dev\check-deps.bat            # Check dependencies
  dev\check-deps.bat --install  # Check and install
```

## Migration Guide

If you were using the old scripts, here's how to migrate:

| Old Script | New Script | Notes |
|------------|------------|-------|
| test_build.bat | scripts\test.bat --component dag | Unified test runner |
| test_go.bat | scripts\test.bat --component agent | Unified test runner |
| test_lib_only.bat | scripts\debug.bat --action libs | Part of debug diagnostics |
| debug_lib.bat | scripts\debug.bat --action imports | Part of debug diagnostics |
| test_progressive.bat | dev\quick-build.bat | For incremental builds |

## Common Tasks

### Building for Production
```bash
scripts\build.bat --clean --phase 3
```

### Quick Development Cycle
```bash
dev\quick-build.bat              # Fast build
scripts\test.bat --component dag # Test changes
scripts\run_node.bat             # Run and test
```

### Troubleshooting Build Issues
```bash
scripts\debug.bat                # Full diagnostics
scripts\debug.bat --action deps  # Check dependencies
dev\clean-build.bat              # Clean rebuild
```

### Running Tests with Coverage
```bash
scripts\test.bat --coverage --verbose
# Coverage reports:
# - Rust: core\coverage\tarpaulin-report.html
# - Go: agent\go\coverage.html
```

## Script Features

All scripts follow consistent patterns:
- **Help Support**: All scripts support `--help` flag
- **Error Handling**: Proper error codes and messages
- **Progress Indicators**: Clear status messages with ✅/❌
- **Consistent Options**: Similar option patterns across scripts
- **Directory Independence**: Scripts work from any directory

## Cleanup Recommendations

The following scripts in the root directory can be safely removed as their functionality has been incorporated into the new consolidated scripts:

- test_build.bat
- test_go.bat
- test_lib_only.bat
- test_node.bat
- test_progressive.bat
- debug_lib.bat

Keep only:
- scripts/ folder (production scripts)
- dev/ folder (development scripts)
- Other essential files (build.rs, etc.)