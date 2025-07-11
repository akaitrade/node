# Building ALT-LEDGER 2030 on Windows

This guide explains how to build the ALT-LEDGER 2030 components on Windows systems.

## Prerequisites

### Required Tools

1. **Visual Studio 2019 or 2022** (Community/Professional/Enterprise)
   - With C++ build tools and CMake support
   - Download: https://visualstudio.microsoft.com/downloads/

2. **Rust** (1.70+)
   - Download: https://rustup.rs/
   - Run: `rustup-init.exe` and follow instructions

3. **Go** (1.19+)
   - Download: https://golang.org/dl/
   - Add to PATH during installation

4. **Node.js** (16+)
   - Download: https://nodejs.org/
   - Includes npm package manager

5. **CMake** (3.15+)
   - Download: https://cmake.org/download/
   - Add to PATH during installation

### Optional Tools

- **Git** for version control
- **Doxygen** for C++ documentation generation
- **PowerShell 5.0+** (recommended for PowerShell build script)

## Build Methods

### Method 1: PowerShell Script (Recommended)

```powershell
# Navigate to project directory
cd credits-alt-ledger-2030

# Basic build
.\scripts\build.ps1

# Advanced build options
.\scripts\build.ps1 -Phase 2 -Type Debug -CreatePackage

# Build with documentation
.\scripts\build.ps1 -GenerateDocs -CreatePackage

# Skip tests for faster build
.\scripts\build.ps1 -NoTests -NoBenchmarks
```

### Method 2: Batch Script

```cmd
REM Navigate to project directory
cd credits-alt-ledger-2030

REM Basic build
scripts\build.bat

REM Advanced build options
scripts\build.bat --phase 2 --type Debug --create-package

REM Build with documentation
scripts\build.bat --generate-docs --create-package

REM Skip tests for faster build
scripts\build.bat --no-tests --no-benchmarks
```

### Method 3: Manual Build

```powershell
# 1. Setup environment
$env:RUSTFLAGS = "-C target-cpu=native"
$env:CGO_ENABLED = "1"

# 2. Build Rust components
cd core
cargo build --release
cd ..

# 3. Build C++ components
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release --parallel
cd ..

# 4. Build Go SDK
cd agent\go
go mod download
go build -v ./...
cd ..\..

# 5. Build TypeScript SDK (if exists)
cd agent\typescript
npm install
npm run build
cd ..\..
```

## Build Options

### PowerShell Script Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `-Phase` | Build phase (1/2/3) | 1 |
| `-Type` | Build type (Debug/Release) | Release |
| `-Jobs` | Parallel job count | Auto-detect |
| `-NoTests` | Skip running tests | false |
| `-NoBenchmarks` | Skip building benchmarks | false |
| `-SkipInstall` | Skip installation step | false |
| `-GenerateDocs` | Generate documentation | false |
| `-CreatePackage` | Create distribution package | false |

### Environment Variables

| Variable | Description | Example |
|----------|-------------|---------|
| `BUILD_TYPE` | Override build type | `Release` |
| `BUILD_JOBS` | Override parallel jobs | `8` |
| `BUILD_PHASE` | Override build phase | `2` |
| `ENABLE_TESTS` | Enable/disable tests | `ON`/`OFF` |
| `ENABLE_BENCHMARKS` | Enable/disable benchmarks | `ON`/`OFF` |

## Database Backends

The ALT-LEDGER 2030 supports two database backends for the DAG storage:

### RocksDB Backend (Default on Linux)
- High-performance LSM-tree storage
- Requires libclang for compilation
- Optimal for production deployments
- Feature flag: `--features rocksdb-backend`

### Sled Backend (Default on Windows)
- Pure Rust embedded database
- No native dependencies (no libclang required)
- Excellent for development and testing
- Feature flag: `--features sled-backend`

The build scripts automatically choose the appropriate backend based on your system configuration.

## Build Phases

### Phase 1: Core Infrastructure
- Rust DAG engine
- Basic C++ networking
- WASM runtime foundation
- Go agent SDK basics

### Phase 2: Advanced Features
- aBFT consensus implementation
- Enhanced networking protocols
- Full CNS integration
- Cross-agent coordination

### Phase 3: Production Features
- Dynamic sharding
- Performance optimizations
- Migration tools
- Complete integration layer

## Output Artifacts

After successful build, find artifacts in:

```
build/
├── lib/                    # Dynamic libraries
├── include/                # C++ headers
├── Debug/ or Release/      # Visual Studio output
└── package/                # Distribution package (if created)

core/target/release/        # Rust static libraries
agent/go/                   # Go SDK binaries
agent/typescript/dist/      # TypeScript SDK (if built)
```

## Troubleshooting

### Common Issues

1. **"Visual Studio not found"**
   - Install Visual Studio with C++ build tools
   - Restart command prompt after installation

2. **"rustc not found"**
   - Install Rust via rustup: https://rustup.rs/
   - Restart command prompt after installation

3. **"Unable to find libclang" (Rust bindgen error)**
   - **Automatic Solution**: Build scripts will automatically use Sled backend instead of RocksDB
   - **Manual Solution**: Install LLVM:
     - Via winget: `winget install LLVM.LLVM`
     - Via chocolatey: `choco install llvm`
     - Download from: https://llvm.org/builds/
   - **Alternative**: Set `LIBCLANG_PATH` environment variable to LLVM bin directory

4. **"CMake configuration failed"**
   - Check Visual Studio installation
   - Try different generator: `-G "Visual Studio 17 2022"`

5. **"Go build failed"**
   - Check Go installation: `go version`
   - Verify internet connection for module downloads

6. **"npm install failed"**
   - Check Node.js installation: `node --version`
   - Clear npm cache: `npm cache clean --force`

### Performance Tips

- Use SSD for faster compilation
- Increase parallel jobs: `-Jobs 16`
- Use Release builds for performance testing
- Skip tests during development: `-NoTests`

### Debug Builds

For debugging and development:

```powershell
# Debug build with symbols
.\scripts\build.ps1 -Type Debug -NoTests

# Enable verbose output
$env:RUST_LOG = "debug"
.\scripts\build.ps1 -Type Debug
```

## Integration with Existing CREDITS

The ALT-LEDGER 2030 is designed to integrate with existing CREDITS blockchain:

1. **Phase 1**: Runs alongside existing CREDITS
2. **Phase 2**: Gradual migration of functionality
3. **Phase 3**: Full replacement (optional)

See `ALT_LEDGER_2030_CREDITS_ARCHITECTURE.md` for detailed integration plans.

## Next Steps

After successful build:

1. Run tests: `.\scripts\build.ps1 -Type Debug`
2. Generate docs: `.\scripts\build.ps1 -GenerateDocs`
3. Create package: `.\scripts\build.ps1 -CreatePackage`
4. Deploy to test environment
5. Begin integration testing

## Support

For build issues:
- Check this guide first
- Review error messages carefully
- Ensure all prerequisites are installed
- Try different build phases/types