#
# CREDITS ALT-LEDGER 2030 Build Script for Windows (PowerShell)
#
# Builds all components of the ALT-LEDGER 2030 architecture
#

[CmdletBinding()]
param(
    [int]$Phase = 1,
    [string]$Type = "Release",
    [int]$Jobs = 0,
    [switch]$NoTests,
    [switch]$NoBenchmarks,
    [switch]$SkipInstall,
    [switch]$GenerateDocs,
    [switch]$CreatePackage,
    [switch]$Help
)

# Show help if requested
if ($Help) {
    Write-Host @"
ALT-LEDGER 2030 Build Script for Windows (PowerShell)

Usage: .\build.ps1 [options]

Options:
  -Phase <1|2|3>       Build phase (default: 1)
  -Type <Debug|Release> Build type (default: Release)
  -Jobs <N>            Number of parallel jobs (default: auto)
  -NoTests             Skip running tests
  -NoBenchmarks        Skip building benchmarks
  -SkipInstall         Skip installation step
  -GenerateDocs        Generate documentation
  -CreatePackage       Create distribution package
  -Help                Show this help message

Environment Variables:
  BUILD_TYPE           Override build type
  BUILD_JOBS           Override number of jobs
  BUILD_PHASE          Override build phase
  ENABLE_TESTS         Enable/disable tests (ON/OFF)
  ENABLE_BENCHMARKS    Enable/disable benchmarks (ON/OFF)

Examples:
  .\build.ps1                   # Basic build
  .\build.ps1 -Phase 2          # Build Phase 2 components
  .\build.ps1 -Type Debug       # Debug build
  .\build.ps1 -NoTests          # Skip tests
  .\build.ps1 -CreatePackage    # Create distribution package
"@
    exit 0
}

# Color functions
function Write-Info($message) {
    Write-Host "[INFO] $message" -ForegroundColor Blue
}

function Write-Success($message) {
    Write-Host "[SUCCESS] $message" -ForegroundColor Green
}

function Write-Warning($message) {
    Write-Host "[WARNING] $message" -ForegroundColor Yellow
}

function Write-Error($message) {
    Write-Host "[ERROR] $message" -ForegroundColor Red
}

# Configuration
$BuildType = if ($env:BUILD_TYPE) { $env:BUILD_TYPE } else { $Type }
$BuildJobs = if ($env:BUILD_JOBS) { [int]$env:BUILD_JOBS } elseif ($Jobs -gt 0) { $Jobs } else { $env:NUMBER_OF_PROCESSORS }
$BuildPhase = if ($env:BUILD_PHASE) { [int]$env:BUILD_PHASE } else { $Phase }
$EnableTests = if ($env:ENABLE_TESTS) { $env:ENABLE_TESTS -eq "ON" } else { -not $NoTests }
$EnableBenchmarks = if ($env:ENABLE_BENCHMARKS) { $env:ENABLE_BENCHMARKS -eq "ON" } else { -not $NoBenchmarks }

# Project paths
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = (Resolve-Path $ProjectRoot).Path
$BuildDir = Join-Path $ProjectRoot "build"

Write-Info "Starting ALT-LEDGER 2030 build process"
Write-Info "Build type: $BuildType"
Write-Info "Build jobs: $BuildJobs"
Write-Info "Build phase: $BuildPhase"
Write-Info "Project root: $ProjectRoot"

# Function to check if a command exists
function Test-Command($command) {
    $null = Get-Command $command -ErrorAction SilentlyContinue
    return $?
}

# Function to get version from command output
function Get-Version($command, $pattern) {
    try {
        $output = & $command --version 2>$null
        if ($output -match $pattern) {
            return $matches[1]
        }
        return "Unknown"
    } catch {
        return "Unknown"
    }
}

# Check prerequisites
function Test-Prerequisites {
    Write-Info "Checking build prerequisites..."
    
    $requiredTools = @("cmake", "rustc", "cargo", "go", "node", "npm")
    $allFound = $true
    
    foreach ($tool in $requiredTools) {
        if (-not (Test-Command $tool)) {
            Write-Error "Required tool '$tool' not found"
            $allFound = $false
        }
    }
    
    if (-not $allFound) {
        throw "Missing required tools"
    }
    
    # Check versions
    $cmakeVersion = Get-Version "cmake" "cmake version (\d+\.\d+)"
    $rustVersion = Get-Version "rustc" "rustc (\d+\.\d+\.\d+)"
    $goVersion = Get-Version "go" "go version go(\d+\.\d+)"
    $nodeVersion = Get-Version "node" "v(\d+\.\d+\.\d+)"
    
    Write-Success "All prerequisites satisfied"
    Write-Info "CMake: $cmakeVersion"
    Write-Info "Rust: $rustVersion"
    Write-Info "Go: $goVersion"
    Write-Info "Node.js: $nodeVersion"
}

# Setup build environment
function Initialize-BuildEnvironment {
    Write-Info "Setting up build environment..."
    
    # Create build directory
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }
    
    # Set environment variables
    $env:RUSTFLAGS = "-C target-cpu=native"
    $env:CGO_ENABLED = "1"
    
    # Try to find and set LIBCLANG_PATH for bindgen
    $clangPaths = @(
        "C:\Program Files\LLVM\bin",
        "C:\Program Files (x86)\LLVM\bin",
        "C:\msys64\mingw64\bin",
        "C:\msys64\clang64\bin",
        "C:\tools\llvm\bin"
    )
    
    $clangFound = $false
    foreach ($path in $clangPaths) {
        $clangDll = Join-Path $path "libclang.dll"
        if (Test-Path $clangDll) {
            $env:LIBCLANG_PATH = $path
            Write-Info "Found libclang at: $path"
            $clangFound = $true
            break
        }
    }
    
    if (-not $clangFound) {
        Write-Warning "libclang not found. Attempting to install via winget..."
        try {
            & winget install LLVM.LLVM
            $env:LIBCLANG_PATH = "C:\Program Files\LLVM\bin"
            Write-Info "LLVM installed via winget"
        } catch {
            Write-Warning "Could not install LLVM automatically. You may need to:"
            Write-Warning "1. Install LLVM from https://llvm.org/builds/"
            Write-Warning "2. Or install via choco: choco install llvm"
            Write-Warning "3. Or set LIBCLANG_PATH manually"
        }
    }
    
    # Try to find and load Visual Studio environment
    $vsVersions = @("2022", "2019", "2017")
    $vsEditions = @("Enterprise", "Professional", "Community")
    $vsLoaded = $false
    
    foreach ($version in $vsVersions) {
        foreach ($edition in $vsEditions) {
            $vsPath = "C:\Program Files\Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vsPath) {
                Write-Info "Loading Visual Studio $version $edition environment..."
                cmd /c "`"$vsPath`" && set" | ForEach-Object {
                    if ($_ -match "^(.*?)=(.*)$") {
                        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
                    }
                }
                $vsLoaded = $true
                break
            }
        }
        if ($vsLoaded) { break }
    }
    
    if (-not $vsLoaded) {
        Write-Warning "Visual Studio not found, using default compiler"
    }
    
    Write-Success "Build environment ready"
}

# Build Rust DAG engine
function Build-DAGEngine {
    Write-Info "Building Rust DAG engine..."
    
    $coreDir = Join-Path $ProjectRoot "core"
    Push-Location $coreDir
    
    try {
        # Use Windows-compatible Cargo.toml if libclang is not available
        $useWindowsConfig = $false
        if (-not $env:LIBCLANG_PATH -or -not (Test-Path (Join-Path $env:LIBCLANG_PATH "libclang.dll"))) {
            Write-Warning "libclang not available, using Windows-compatible configuration..."
            if (Test-Path "Cargo.windows.toml") {
                Copy-Item "Cargo.windows.toml" "Cargo.toml" -Force
                $useWindowsConfig = $true
                Write-Info "Using Sled database backend instead of RocksDB"
            }
        }
        
        # Clean previous build
        & cargo clean
        if ($LASTEXITCODE -ne 0) { throw "Cargo clean failed" }
        
        # Build in release mode with Windows-compatible features
        if ($useWindowsConfig) {
            & cargo build --release --features sled-backend
        } else {
            & cargo build --release --features rocksdb-backend
        }
        if ($LASTEXITCODE -ne 0) { throw "Cargo build failed" }
        
        # Run tests if enabled
        if ($EnableTests) {
            Write-Info "Running DAG engine tests..."
            if ($useWindowsConfig) {
                & cargo test --release --features sled-backend
            } else {
                & cargo test --release --features rocksdb-backend
            }
            if ($LASTEXITCODE -ne 0) { throw "DAG engine tests failed" }
        }
        
        Write-Success "DAG engine built successfully"
    } finally {
        Pop-Location
    }
}

# Build C++ components
function Build-CppComponents {
    Write-Info "Building C++ components..."
    
    Push-Location $BuildDir
    
    try {
        # Configure CMake
        $cmakeArgs = @(
            "-DCMAKE_BUILD_TYPE=$BuildType",
            "-DBUILD_PHASE1=ON",
            "-DBUILD_TESTS=$(if ($EnableTests) { 'ON' } else { 'OFF' })",
            "-DBUILD_BENCHMARKS=$(if ($EnableBenchmarks) { 'ON' } else { 'OFF' })"
        )
        
        if ($BuildPhase -eq 2) {
            $cmakeArgs += "-DBUILD_PHASE2=ON"
        } elseif ($BuildPhase -eq 3) {
            $cmakeArgs += @("-DBUILD_PHASE2=ON", "-DBUILD_PHASE3=ON")
        }
        
        # Add Windows-specific generator
        $cmakeArgs += @("-G", "Visual Studio 16 2019", "-A", "x64")
        
        & cmake $cmakeArgs $ProjectRoot
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }
        
        # Build
        & cmake --build . --config $BuildType --parallel $BuildJobs
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed" }
        
        # Run tests if enabled
        if ($EnableTests) {
            Write-Info "Running C++ tests..."
            & ctest --build-config $BuildType --parallel $BuildJobs
            if ($LASTEXITCODE -ne 0) { throw "C++ tests failed" }
        }
        
        Write-Success "C++ components built successfully"
    } finally {
        Pop-Location
    }
}

# Build Go Agent SDK
function Build-AgentSDK {
    Write-Info "Building Go Agent SDK..."
    
    $goDir = Join-Path $ProjectRoot "agent\go"
    Push-Location $goDir
    
    try {
        # Download dependencies
        & go mod download
        if ($LASTEXITCODE -ne 0) { throw "Failed to download Go dependencies" }
        
        # Build
        & go build -v ./...
        if ($LASTEXITCODE -ne 0) { throw "Go Agent SDK build failed" }
        
        # Run tests if enabled
        if ($EnableTests) {
            Write-Info "Running Go tests..."
            & go test -v ./...
            if ($LASTEXITCODE -ne 0) { throw "Go tests failed" }
        }
        
        Write-Success "Go Agent SDK built successfully"
    } finally {
        Pop-Location
    }
}

# Build TypeScript SDK
function Build-TypeScriptSDK {
    Write-Info "Building TypeScript Agent SDK..."
    
    $tsDir = Join-Path $ProjectRoot "agent\typescript"
    if (Test-Path $tsDir) {
        Push-Location $tsDir
        
        try {
            # Install dependencies
            & npm install
            if ($LASTEXITCODE -ne 0) { throw "Failed to install TypeScript dependencies" }
            
            # Build
            & npm run build
            if ($LASTEXITCODE -ne 0) { throw "TypeScript SDK build failed" }
            
            # Run tests if enabled
            if ($EnableTests) {
                Write-Info "Running TypeScript tests..."
                & npm test
                if ($LASTEXITCODE -ne 0) { throw "TypeScript tests failed" }
            }
            
            Write-Success "TypeScript Agent SDK built successfully"
        } finally {
            Pop-Location
        }
    } else {
        Write-Warning "TypeScript SDK not found, skipping"
    }
}

# Build benchmarks
function Build-Benchmarks {
    if ($EnableBenchmarks) {
        Write-Info "Building performance benchmarks..."
        
        Push-Location $BuildDir
        
        try {
            & cmake --build . --target benchmarks --config $BuildType
            if ($LASTEXITCODE -ne 0) { throw "Benchmark build failed" }
            
            Write-Success "Benchmarks built successfully"
        } finally {
            Pop-Location
        }
    }
}

# Install components
function Install-Components {
    Write-Info "Installing components..."
    
    Push-Location $BuildDir
    
    try {
        & cmake --install . --config $BuildType
        if ($LASTEXITCODE -ne 0) { throw "Installation failed" }
        
        Write-Success "Components installed successfully"
    } finally {
        Pop-Location
    }
}

# Generate documentation
function New-Documentation {
    Write-Info "Generating documentation..."
    
    # Rust docs
    $coreDir = Join-Path $ProjectRoot "core"
    if (Test-Command "cargo") {
        Push-Location $coreDir
        try {
            & cargo doc --no-deps --release
        } finally {
            Pop-Location
        }
    }
    
    # C++ docs (if Doxygen is available)
    if (Test-Command "doxygen") {
        $doxyfile = Join-Path $ProjectRoot "Doxyfile"
        if (Test-Path $doxyfile) {
            Push-Location $ProjectRoot
            try {
                & doxygen $doxyfile
            } finally {
                Pop-Location
            }
        }
    }
    
    # Go docs
    $goDir = Join-Path $ProjectRoot "agent\go"
    if (Test-Command "godoc") {
        Push-Location $goDir
        try {
            $docsPath = Join-Path $BuildDir "go-docs.txt"
            & go doc -all | Out-File -FilePath $docsPath -Encoding UTF8
        } finally {
            Pop-Location
        }
    }
    
    Write-Success "Documentation generated"
}

# Create distribution package
function New-Package {
    Write-Info "Creating distribution package..."
    
    $packageDir = Join-Path $BuildDir "package"
    $version = "1.0.0"
    
    if (-not (Test-Path $packageDir)) {
        New-Item -ItemType Directory -Path $packageDir | Out-Null
    }
    
    # Copy binaries
    $possibleDlls = @(
        (Join-Path $BuildDir "lib\credits_alt_ledger.dll"),
        (Join-Path $BuildDir "$BuildType\credits_alt_ledger.dll")
    )
    
    foreach ($dll in $possibleDlls) {
        if (Test-Path $dll) {
            Copy-Item $dll $packageDir
        }
    }
    
    # Copy headers
    $includeDir = Join-Path $BuildDir "include"
    if (Test-Path $includeDir) {
        $packageInclude = Join-Path $packageDir "include"
        Copy-Item $includeDir $packageInclude -Recurse
    }
    
    # Copy Rust library
    $rustLib = Join-Path $ProjectRoot "core\target\release\dag_engine.lib"
    if (Test-Path $rustLib) {
        Copy-Item $rustLib $packageDir
    }
    
    # Copy Go SDK
    $goPackageDir = Join-Path $packageDir "agent\go"
    if (-not (Test-Path $goPackageDir)) {
        New-Item -ItemType Directory -Path $goPackageDir -Force | Out-Null
    }
    $goSrcDir = Join-Path $ProjectRoot "agent\go"
    Copy-Item "$goSrcDir\*" $goPackageDir -Recurse
    
    # Copy TypeScript SDK if it exists
    $tsDistDir = Join-Path $ProjectRoot "agent\typescript\dist"
    if (Test-Path $tsDistDir) {
        $tsPackageDir = Join-Path $packageDir "agent\typescript"
        Copy-Item $tsDistDir $tsPackageDir -Recurse
    }
    
    # Copy documentation
    $docsDir = Join-Path $ProjectRoot "target\doc"
    if (Test-Path $docsDir) {
        $docsPackageDir = Join-Path $packageDir "rust-docs"
        Copy-Item $docsDir $docsPackageDir -Recurse
    }
    
    # Create archive
    $archiveName = "credits-alt-ledger-2030-$version.zip"
    $archivePath = Join-Path $BuildDir $archiveName
    
    try {
        Compress-Archive -Path "$packageDir\*" -DestinationPath $archivePath -Force
        Write-Success "Distribution package created: $archiveName"
    } catch {
        Write-Warning "Failed to create zip archive: $_"
        Write-Warning "Package directory available: $packageDir"
    }
}

# Main execution
try {
    $startTime = Get-Date
    
    Test-Prerequisites
    Initialize-BuildEnvironment
    
    Build-DAGEngine
    Build-CppComponents
    Build-AgentSDK
    Build-TypeScriptSDK
    Build-Benchmarks
    
    if (-not $SkipInstall) {
        Install-Components
    }
    
    if ($GenerateDocs) {
        New-Documentation
    }
    
    if ($CreatePackage) {
        New-Package
    }
    
    $endTime = Get-Date
    $buildTime = $endTime - $startTime
    
    Write-Success "ALT-LEDGER 2030 build completed successfully!"
    Write-Info "Build artifacts available in: $BuildDir"
    
    # Print build summary
    Write-Host ""
    Write-Host "=== Build Summary ==="
    Write-Host "Build Type: $BuildType"
    Write-Host "Build Phase: $BuildPhase"
    Write-Host "Tests: $(if ($EnableTests) { 'Enabled' } else { 'Disabled' })"
    Write-Host "Benchmarks: $(if ($EnableBenchmarks) { 'Enabled' } else { 'Disabled' })"
    Write-Host "Build Time: $($buildTime.ToString('hh\:mm\:ss'))"
    Write-Host "Completed: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Write-Host "===================="
    
} catch {
    Write-Error "Build failed: $_"
    exit 1
}