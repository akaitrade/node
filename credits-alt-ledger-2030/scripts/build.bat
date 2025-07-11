@echo off
setlocal enabledelayedexpansion

REM
REM CREDITS ALT-LEDGER 2030 Build Script for Windows
REM
REM Builds all components of the ALT-LEDGER 2030 architecture
REM

REM Colors for output (using Windows color codes)
set "RED=[91m"
set "GREEN=[92m"
set "YELLOW=[93m"
set "BLUE=[94m"
set "NC=[0m"

REM Configuration
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release
if "%BUILD_JOBS%"=="" (
    for /f "tokens=*" %%i in ('wmic cpu get NumberOfLogicalProcessors /value ^| find "="') do set %%i
    set BUILD_JOBS=!NumberOfLogicalProcessors!
)
if "%BUILD_PHASE%"=="" set BUILD_PHASE=3
if "%ENABLE_TESTS%"=="" set ENABLE_TESTS=ON
if "%ENABLE_BENCHMARKS%"=="" set ENABLE_BENCHMARKS=ON

REM Project root
set "PROJECT_ROOT=%~dp0.."
for %%i in ("%PROJECT_ROOT%") do set "PROJECT_ROOT=%%~fi"
set "BUILD_DIR=%PROJECT_ROOT%\build"

echo %BLUE%[INFO]%NC% Starting ALT-LEDGER 2030 build process
echo %BLUE%[INFO]%NC% Build type: %BUILD_TYPE%
echo %BLUE%[INFO]%NC% Build jobs: %BUILD_JOBS%
echo %BLUE%[INFO]%NC% Build phase: %BUILD_PHASE%
echo %BLUE%[INFO]%NC% Project root: %PROJECT_ROOT%

REM Check prerequisites
call :check_prerequisites
if %errorlevel% neq 0 exit /b %errorlevel%

REM Setup build environment
call :setup_build_env
if %errorlevel% neq 0 exit /b %errorlevel%

REM Build components
call :build_dag_engine
if %errorlevel% neq 0 exit /b %errorlevel%

call :build_cpp_components
if %errorlevel% neq 0 exit /b %errorlevel%

call :build_agent_sdk
if %errorlevel% neq 0 exit /b %errorlevel%

call :build_typescript_sdk
if %errorlevel% neq 0 exit /b %errorlevel%

call :build_benchmarks
if %errorlevel% neq 0 exit /b %errorlevel%

REM Optional steps
if not "%SKIP_INSTALL%"=="true" (
    call :install_components
    if %errorlevel% neq 0 exit /b %errorlevel%
)

if "%GENERATE_DOCS%"=="true" (
    call :generate_docs
    if %errorlevel% neq 0 exit /b %errorlevel%
)

if "%CREATE_PACKAGE%"=="true" (
    call :create_package
    if %errorlevel% neq 0 exit /b %errorlevel%
)

echo %GREEN%[SUCCESS]%NC% ALT-LEDGER 2030 build completed successfully!
echo %BLUE%[INFO]%NC% Build artifacts available in: %BUILD_DIR%

REM Print build summary
echo.
echo === Build Summary ===
echo Build Type: %BUILD_TYPE%
echo Build Phase: %BUILD_PHASE%
if "%ENABLE_TESTS%"=="ON" (
    echo Tests: Enabled
) else (
    echo Tests: Disabled
)
if "%ENABLE_BENCHMARKS%"=="ON" (
    echo Benchmarks: Enabled
) else (
    echo Benchmarks: Disabled
)
echo Build Time: %date% %time%
echo ==================

exit /b 0

REM ===== FUNCTIONS =====

:check_prerequisites
echo %BLUE%[INFO]%NC% Checking build prerequisites...

REM Check for required tools
set "required_tools=cmake rustc cargo go node npm"
for %%t in (%required_tools%) do (
    where %%t >nul 2>&1
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% Required tool '%%t' not found
        exit /b 1
    )
)

REM Check CMake version
for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr /r "cmake version"') do (
    set cmake_version=%%v
    goto :cmake_version_found
)
:cmake_version_found

REM Check Rust version
for /f "tokens=2" %%v in ('rustc --version 2^>nul') do (
    set rust_version=%%v
    goto :rust_version_found
)
:rust_version_found

REM Check Go version
for /f "tokens=3" %%v in ('go version 2^>nul') do (
    set go_version=%%v
    set go_version=!go_version:go=!
    goto :go_version_found
)
:go_version_found

REM Check Node.js version
for /f "tokens=1" %%v in ('node --version 2^>nul') do (
    set node_version=%%v
    set node_version=!node_version:v=!
    goto :node_version_found
)
:node_version_found

echo %GREEN%[SUCCESS]%NC% All prerequisites satisfied
echo %BLUE%[INFO]%NC% CMake: %cmake_version%
echo %BLUE%[INFO]%NC% Rust: %rust_version%
echo %BLUE%[INFO]%NC% Go: %go_version%
echo %BLUE%[INFO]%NC% Node.js: %node_version%
exit /b 0

:setup_build_env
echo %BLUE%[INFO]%NC% Setting up build environment...

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

REM Set environment variables
set RUSTFLAGS=-C target-cpu=native
set CGO_ENABLED=1

REM Try to find and set LIBCLANG_PATH for bindgen
set CLANG_FOUND=false

for %%p in ("C:\Program Files\LLVM\bin" "C:\Program Files (x86)\LLVM\bin" "C:\msys64\mingw64\bin" "C:\msys64\clang64\bin" "C:\tools\llvm\bin") do (
    if exist "%%~p\libclang.dll" (
        set LIBCLANG_PATH=%%~p
        echo %BLUE%[INFO]%NC% Found libclang at: %%~p
        set CLANG_FOUND=true
        goto :clang_found
    )
)

:clang_found
if "%CLANG_FOUND%"=="false" (
    echo %YELLOW%[WARNING]%NC% libclang not found. Attempting to install via winget...
    winget install LLVM.LLVM >nul 2>&1
    if %errorlevel% equ 0 (
        set LIBCLANG_PATH=C:\Program Files\LLVM\bin
        echo %BLUE%[INFO]%NC% LLVM installed via winget
    ) else (
        echo %YELLOW%[WARNING]%NC% Could not install LLVM automatically. You may need to:
        echo %YELLOW%[WARNING]%NC% 1. Install LLVM from https://llvm.org/builds/
        echo %YELLOW%[WARNING]%NC% 2. Or install via choco: choco install llvm
        echo %YELLOW%[WARNING]%NC% 3. Or set LIBCLANG_PATH manually
    )
)

REM Set Visual Studio environment if available
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2019 environment loaded
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2019 environment loaded
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2019 environment loaded
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2022 environment loaded
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2022 environment loaded
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    echo %BLUE%[INFO]%NC% Visual Studio 2022 environment loaded
) else (
    echo %YELLOW%[WARNING]%NC% Visual Studio not found, using default compiler
)

echo %GREEN%[SUCCESS]%NC% Build environment ready
exit /b 0

:build_dag_engine
echo %BLUE%[INFO]%NC% Building Rust DAG engine...

cd /d "%PROJECT_ROOT%\core"

REM Check if we need to use Windows-compatible configuration
set USE_WINDOWS_CONFIG=false
if "%LIBCLANG_PATH%"=="" (
    set USE_WINDOWS_CONFIG=true
) else (
    if not exist "%LIBCLANG_PATH%\libclang.dll" (
        set USE_WINDOWS_CONFIG=true
    )
)

if "%USE_WINDOWS_CONFIG%"=="true" (
    echo %YELLOW%[WARNING]%NC% libclang not available, using Windows-compatible configuration...
    if exist "Cargo.windows.toml" (
        copy "Cargo.windows.toml" "Cargo.toml" /Y >nul
        echo %BLUE%[INFO]%NC% Using Sled database backend instead of RocksDB
    )
)

REM Clean previous build
cargo clean

REM Build in release mode with appropriate features
if "%USE_WINDOWS_CONFIG%"=="true" (
    cargo build --release --features sled-backend
) else (
    cargo build --release --features rocksdb-backend
)
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% Failed to build DAG engine
    exit /b 1
)

REM Run tests
if "%ENABLE_TESTS%"=="ON" (
    echo %BLUE%[INFO]%NC% Running DAG engine tests...
    if "%USE_WINDOWS_CONFIG%"=="true" (
        cargo test --release --features sled-backend
    ) else (
        cargo test --release --features rocksdb-backend
    )
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% DAG engine tests failed
        exit /b 1
    )
)

echo %GREEN%[SUCCESS]%NC% DAG engine built successfully
cd /d "%BUILD_DIR%"
exit /b 0

:build_cpp_components
echo %BLUE%[INFO]%NC% Building C++ components...

cd /d "%BUILD_DIR%"

REM Configure CMake
set cmake_args=-DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DBUILD_PHASE1=ON -DBUILD_TESTS=%ENABLE_TESTS% -DBUILD_BENCHMARKS=%ENABLE_BENCHMARKS%

if "%BUILD_PHASE%"=="2" (
    set cmake_args=%cmake_args% -DBUILD_PHASE2=ON
) else if "%BUILD_PHASE%"=="3" (
    set cmake_args=%cmake_args% -DBUILD_PHASE2=ON -DBUILD_PHASE3=ON
)

REM Add Windows-specific flags
set cmake_args=%cmake_args% -G "Visual Studio 16 2019" -A x64

cmake %cmake_args% "%PROJECT_ROOT%"
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% CMake configuration failed
    exit /b 1
)

REM Build
cmake --build . --config %BUILD_TYPE% --parallel %BUILD_JOBS%
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% C++ build failed
    exit /b 1
)

REM Run tests
if "%ENABLE_TESTS%"=="ON" (
    echo %BLUE%[INFO]%NC% Running C++ tests...
    ctest --build-config %BUILD_TYPE% --parallel %BUILD_JOBS%
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% C++ tests failed
        exit /b 1
    )
)

echo %GREEN%[SUCCESS]%NC% C++ components built successfully
exit /b 0

:build_agent_sdk
echo %BLUE%[INFO]%NC% Building Go Agent SDK...

cd /d "%PROJECT_ROOT%\agent\go"

REM Download dependencies
go mod download
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% Failed to download Go dependencies
    exit /b 1
)

REM Build
go build -v ./...
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% Go Agent SDK build failed
    exit /b 1
)

REM Run tests
if "%ENABLE_TESTS%"=="ON" (
    echo %BLUE%[INFO]%NC% Running Go tests...
    go test -v ./...
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% Go tests failed
        exit /b 1
    )
)

echo %GREEN%[SUCCESS]%NC% Go Agent SDK built successfully
cd /d "%BUILD_DIR%"
exit /b 0

:build_typescript_sdk
echo %BLUE%[INFO]%NC% Building TypeScript Agent SDK...

if exist "%PROJECT_ROOT%\agent\typescript" (
    cd /d "%PROJECT_ROOT%\agent\typescript"
    
    REM Install dependencies
    npm install
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% Failed to install TypeScript dependencies
        exit /b 1
    )
    
    REM Build
    npm run build
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% TypeScript SDK build failed
        exit /b 1
    )
    
    REM Run tests
    if "%ENABLE_TESTS%"=="ON" (
        echo %BLUE%[INFO]%NC% Running TypeScript tests...
        npm test
        if %errorlevel% neq 0 (
            echo %RED%[ERROR]%NC% TypeScript tests failed
            exit /b 1
        )
    )
    
    echo %GREEN%[SUCCESS]%NC% TypeScript Agent SDK built successfully
    cd /d "%BUILD_DIR%"
) else (
    echo %YELLOW%[WARNING]%NC% TypeScript SDK not found, skipping
)
exit /b 0

:build_benchmarks
if "%ENABLE_BENCHMARKS%"=="ON" (
    echo %BLUE%[INFO]%NC% Building performance benchmarks...
    
    cd /d "%BUILD_DIR%"
    
    cmake --build . --target benchmarks --config %BUILD_TYPE%
    if %errorlevel% neq 0 (
        echo %RED%[ERROR]%NC% Benchmark build failed
        exit /b 1
    )
    
    echo %GREEN%[SUCCESS]%NC% Benchmarks built successfully
)
exit /b 0

:install_components
echo %BLUE%[INFO]%NC% Installing components...

cd /d "%BUILD_DIR%"

cmake --install . --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo %RED%[ERROR]%NC% Installation failed
    exit /b 1
)

echo %GREEN%[SUCCESS]%NC% Components installed successfully
exit /b 0

:generate_docs
echo %BLUE%[INFO]%NC% Generating documentation...

REM Rust docs
cd /d "%PROJECT_ROOT%\core"
where cargo >nul 2>&1
if %errorlevel% equ 0 (
    cargo doc --no-deps --release
)

REM C++ docs (if Doxygen is available)
where doxygen >nul 2>&1
if %errorlevel% equ 0 (
    if exist "%PROJECT_ROOT%\Doxyfile" (
        cd /d "%PROJECT_ROOT%"
        doxygen Doxyfile
    )
)

REM Go docs
cd /d "%PROJECT_ROOT%\agent\go"
where godoc >nul 2>&1
if %errorlevel% equ 0 (
    go doc -all > "%BUILD_DIR%\go-docs.txt"
)

echo %GREEN%[SUCCESS]%NC% Documentation generated
cd /d "%BUILD_DIR%"
exit /b 0

:create_package
echo %BLUE%[INFO]%NC% Creating distribution package...

set "package_dir=%BUILD_DIR%\package"
set "version=1.0.0"

if not exist "%package_dir%" mkdir "%package_dir%"

REM Copy binaries
if exist "%BUILD_DIR%\lib\credits_alt_ledger.dll" (
    copy "%BUILD_DIR%\lib\credits_alt_ledger.dll" "%package_dir%\"
)
if exist "%BUILD_DIR%\%BUILD_TYPE%\credits_alt_ledger.dll" (
    copy "%BUILD_DIR%\%BUILD_TYPE%\credits_alt_ledger.dll" "%package_dir%\"
)

REM Copy headers
if exist "%BUILD_DIR%\include" (
    xcopy "%BUILD_DIR%\include" "%package_dir%\include" /e /i /h /y
)

REM Copy Rust library
if exist "%PROJECT_ROOT%\core\target\release\dag_engine.lib" (
    copy "%PROJECT_ROOT%\core\target\release\dag_engine.lib" "%package_dir%\"
)

REM Copy Go SDK
if not exist "%package_dir%\agent\go" mkdir "%package_dir%\agent\go"
xcopy "%PROJECT_ROOT%\agent\go\*" "%package_dir%\agent\go\" /e /i /h /y

REM Copy TypeScript SDK if it exists
if exist "%PROJECT_ROOT%\agent\typescript\dist" (
    if not exist "%package_dir%\agent\typescript" mkdir "%package_dir%\agent\typescript"
    xcopy "%PROJECT_ROOT%\agent\typescript\dist" "%package_dir%\agent\typescript\" /e /i /h /y
)

REM Copy documentation
if exist "%PROJECT_ROOT%\target\doc" (
    xcopy "%PROJECT_ROOT%\target\doc" "%package_dir%\rust-docs\" /e /i /h /y
)

REM Create archive using PowerShell
cd /d "%BUILD_DIR%"
powershell -command "Compress-Archive -Path '%package_dir%\*' -DestinationPath 'credits-alt-ledger-2030-%version%.zip' -Force"
if %errorlevel% equ 0 (
    echo %GREEN%[SUCCESS]%NC% Distribution package created: credits-alt-ledger-2030-%version%.zip
) else (
    echo %YELLOW%[WARNING]%NC% Failed to create zip archive, package directory available: %package_dir%
)

exit /b 0

REM ===== COMMAND LINE ARGUMENT PROCESSING =====

:parse_args
if "%~1"=="" goto :args_done

if "%~1"=="--phase" (
    set BUILD_PHASE=%~2
    shift
    shift
    goto :parse_args
)

if "%~1"=="--type" (
    set BUILD_TYPE=%~2
    shift
    shift
    goto :parse_args
)

if "%~1"=="--jobs" (
    set BUILD_JOBS=%~2
    shift
    shift
    goto :parse_args
)

if "%~1"=="--no-tests" (
    set ENABLE_TESTS=OFF
    shift
    goto :parse_args
)

if "%~1"=="--no-benchmarks" (
    set ENABLE_BENCHMARKS=OFF
    shift
    goto :parse_args
)

if "%~1"=="--skip-install" (
    set SKIP_INSTALL=true
    shift
    goto :parse_args
)

if "%~1"=="--generate-docs" (
    set GENERATE_DOCS=true
    shift
    goto :parse_args
)

if "%~1"=="--create-package" (
    set CREATE_PACKAGE=true
    shift
    goto :parse_args
)

if "%~1"=="--help" (
    echo ALT-LEDGER 2030 Build Script for Windows
    echo.
    echo Usage: %~nx0 [options]
    echo.
    echo Options:
    echo   --phase ^<1^|2^|3^>      Build phase (default: 1)
    echo   --type ^<Debug^|Release^>  Build type (default: Release)
    echo   --jobs ^<N^>           Number of parallel jobs (default: auto)
    echo   --no-tests           Skip running tests
    echo   --no-benchmarks      Skip building benchmarks
    echo   --skip-install       Skip installation step
    echo   --generate-docs      Generate documentation
    echo   --create-package     Create distribution package
    echo   --help               Show this help message
    echo.
    echo Environment Variables:
    echo   BUILD_TYPE           Override build type
    echo   BUILD_JOBS           Override number of jobs
    echo   BUILD_PHASE          Override build phase
    echo   ENABLE_TESTS         Enable/disable tests (ON/OFF)
    echo   ENABLE_BENCHMARKS    Enable/disable benchmarks (ON/OFF)
    echo.
    echo Examples:
    echo   %~nx0                   # Basic build
    echo   %~nx0 --phase 2         # Build Phase 2 components
    echo   %~nx0 --type Debug      # Debug build
    echo   %~nx0 --no-tests        # Skip tests
    echo   %~nx0 --create-package  # Create distribution package
    exit /b 0
)

echo %RED%[ERROR]%NC% Unknown option: %~1
exit /b 1

:args_done

REM Process command line arguments
call :parse_args %*