#!/bin/bash

#
# CREDITS ALT-LEDGER 2030 Build Script
#
# Builds all components of the ALT-LEDGER 2030 architecture
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Configuration
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
BUILD_PHASE="${BUILD_PHASE:-1}"
ENABLE_TESTS="${ENABLE_TESTS:-ON}"
ENABLE_BENCHMARKS="${ENABLE_BENCHMARKS:-ON}"

# Project root
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

log_info "Starting ALT-LEDGER 2030 build process"
log_info "Build type: ${BUILD_TYPE}"
log_info "Build jobs: ${BUILD_JOBS}"
log_info "Build phase: ${BUILD_PHASE}"
log_info "Project root: ${PROJECT_ROOT}"

# Check prerequisites
check_prerequisites() {
    log_info "Checking build prerequisites..."
    
    # Check for required tools
    local required_tools=("cmake" "rustc" "cargo" "go" "node" "npm")
    for tool in "${required_tools[@]}"; do
        if ! command -v "$tool" &> /dev/null; then
            log_error "Required tool '$tool' not found"
            exit 1
        fi
    done
    
    # Check versions
    cmake_version=$(cmake --version | head -n1 | grep -o '[0-9]\+\.[0-9]\+' | head -n1)
    if [[ $(echo "$cmake_version >= 3.15" | bc -l) -eq 0 ]]; then
        log_error "CMake 3.15+ required, found $cmake_version"
        exit 1
    fi
    
    rustc_version=$(rustc --version | grep -o '[0-9]\+\.[0-9]\+' | head -n1)
    if [[ $(echo "$rustc_version >= 1.70" | bc -l) -eq 0 ]]; then
        log_error "Rust 1.70+ required, found $rustc_version"
        exit 1
    fi
    
    go_version=$(go version | grep -o 'go[0-9]\+\.[0-9]\+' | grep -o '[0-9]\+\.[0-9]\+')
    if [[ $(echo "$go_version >= 1.19" | bc -l) -eq 0 ]]; then
        log_error "Go 1.19+ required, found $go_version"
        exit 1
    fi
    
    log_success "All prerequisites satisfied"
}

# Setup build environment
setup_build_env() {
    log_info "Setting up build environment..."
    
    # Create build directory
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Set environment variables
    export RUSTFLAGS="-C target-cpu=native"
    export CGO_ENABLED=1
    
    log_success "Build environment ready"
}

# Build Rust DAG engine
build_dag_engine() {
    log_info "Building Rust DAG engine..."
    
    cd "${PROJECT_ROOT}/core"
    
    # Clean previous build
    cargo clean
    
    # Build in release mode
    if ! cargo build --release; then
        log_error "Failed to build DAG engine"
        exit 1
    fi
    
    # Generate C headers
    if ! cargo build --release; then
        log_error "Failed to generate C headers"
        exit 1
    fi
    
    # Run tests
    if [[ "$ENABLE_TESTS" == "ON" ]]; then
        log_info "Running DAG engine tests..."
        if ! cargo test --release; then
            log_error "DAG engine tests failed"
            exit 1
        fi
    fi
    
    log_success "DAG engine built successfully"
}

# Build C++ components
build_cpp_components() {
    log_info "Building C++ components..."
    
    cd "$BUILD_DIR"
    
    # Configure CMake
    local cmake_args=(
        "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
        "-DBUILD_PHASE1=ON"
        "-DBUILD_TESTS=$ENABLE_TESTS"
        "-DBUILD_BENCHMARKS=$ENABLE_BENCHMARKS"
    )
    
    if [[ "$BUILD_PHASE" == "2" ]]; then
        cmake_args+=("-DBUILD_PHASE2=ON")
    elif [[ "$BUILD_PHASE" == "3" ]]; then
        cmake_args+=("-DBUILD_PHASE2=ON" "-DBUILD_PHASE3=ON")
    fi
    
    if ! cmake "${cmake_args[@]}" "$PROJECT_ROOT"; then
        log_error "CMake configuration failed"
        exit 1
    fi
    
    # Build
    if ! cmake --build . --config "$BUILD_TYPE" -j "$BUILD_JOBS"; then
        log_error "C++ build failed"
        exit 1
    fi
    
    # Run tests
    if [[ "$ENABLE_TESTS" == "ON" ]]; then
        log_info "Running C++ tests..."
        if ! ctest --build-config "$BUILD_TYPE" --parallel "$BUILD_JOBS"; then
            log_error "C++ tests failed"
            exit 1
        fi
    fi
    
    log_success "C++ components built successfully"
}

# Build Go Agent SDK
build_agent_sdk() {
    log_info "Building Go Agent SDK..."
    
    cd "${PROJECT_ROOT}/agent/go"
    
    # Download dependencies
    if ! go mod download; then
        log_error "Failed to download Go dependencies"
        exit 1
    fi
    
    # Build
    if ! go build -v ./...; then
        log_error "Go Agent SDK build failed"
        exit 1
    fi
    
    # Run tests
    if [[ "$ENABLE_TESTS" == "ON" ]]; then
        log_info "Running Go tests..."
        if ! go test -v ./...; then
            log_error "Go tests failed"
            exit 1
        fi
    fi
    
    log_success "Go Agent SDK built successfully"
}

# Build TypeScript Agent SDK
build_typescript_sdk() {
    log_info "Building TypeScript Agent SDK..."
    
    if [[ -d "${PROJECT_ROOT}/agent/typescript" ]]; then
        cd "${PROJECT_ROOT}/agent/typescript"
        
        # Install dependencies
        if ! npm install; then
            log_error "Failed to install TypeScript dependencies"
            exit 1
        fi
        
        # Build
        if ! npm run build; then
            log_error "TypeScript SDK build failed"
            exit 1
        fi
        
        # Run tests
        if [[ "$ENABLE_TESTS" == "ON" ]]; then
            log_info "Running TypeScript tests..."
            if ! npm test; then
                log_error "TypeScript tests failed"
                exit 1
            fi
        fi
        
        log_success "TypeScript Agent SDK built successfully"
    else
        log_warning "TypeScript SDK not found, skipping"
    fi
}

# Build benchmarks
build_benchmarks() {
    if [[ "$ENABLE_BENCHMARKS" == "ON" ]]; then
        log_info "Building performance benchmarks..."
        
        cd "$BUILD_DIR"
        
        if ! cmake --build . --target benchmarks --config "$BUILD_TYPE"; then
            log_error "Benchmark build failed"
            exit 1
        fi
        
        log_success "Benchmarks built successfully"
    fi
}

# Install components
install_components() {
    log_info "Installing components..."
    
    cd "$BUILD_DIR"
    
    if ! cmake --install . --config "$BUILD_TYPE"; then
        log_error "Installation failed"
        exit 1
    fi
    
    log_success "Components installed successfully"
}

# Generate documentation
generate_docs() {
    log_info "Generating documentation..."
    
    # Rust docs
    cd "${PROJECT_ROOT}/core"
    if command -v cargo &> /dev/null; then
        cargo doc --no-deps --release
    fi
    
    # C++ docs (if Doxygen is available)
    if command -v doxygen &> /dev/null && [[ -f "${PROJECT_ROOT}/Doxyfile" ]]; then
        cd "$PROJECT_ROOT"
        doxygen Doxyfile
    fi
    
    # Go docs
    cd "${PROJECT_ROOT}/agent/go"
    if command -v godoc &> /dev/null; then
        go doc -all > "${BUILD_DIR}/go-docs.txt"
    fi
    
    log_success "Documentation generated"
}

# Create distribution package
create_package() {
    log_info "Creating distribution package..."
    
    local package_dir="${BUILD_DIR}/package"
    local version="1.0.0"
    
    mkdir -p "$package_dir"
    
    # Copy binaries
    if [[ -f "${BUILD_DIR}/lib/libcredits_alt_ledger.so" ]]; then
        cp "${BUILD_DIR}/lib/libcredits_alt_ledger.so" "$package_dir/"
    fi
    
    # Copy headers
    if [[ -d "${BUILD_DIR}/include" ]]; then
        cp -r "${BUILD_DIR}/include" "$package_dir/"
    fi
    
    # Copy Rust library
    if [[ -f "${PROJECT_ROOT}/core/target/release/libdag_engine.a" ]]; then
        cp "${PROJECT_ROOT}/core/target/release/libdag_engine.a" "$package_dir/"
    fi
    
    # Copy Go SDK
    mkdir -p "${package_dir}/agent/go"
    cp -r "${PROJECT_ROOT}/agent/go"/* "${package_dir}/agent/go/"
    
    # Copy TypeScript SDK if it exists
    if [[ -d "${PROJECT_ROOT}/agent/typescript/dist" ]]; then
        mkdir -p "${package_dir}/agent/typescript"
        cp -r "${PROJECT_ROOT}/agent/typescript/dist" "${package_dir}/agent/typescript/"
    fi
    
    # Copy documentation
    if [[ -d "${PROJECT_ROOT}/target/doc" ]]; then
        cp -r "${PROJECT_ROOT}/target/doc" "${package_dir}/rust-docs"
    fi
    
    # Create archive
    cd "${BUILD_DIR}"
    tar -czf "credits-alt-ledger-2030-${version}.tar.gz" package/
    
    log_success "Distribution package created: credits-alt-ledger-2030-${version}.tar.gz"
}

# Main build process
main() {
    check_prerequisites
    setup_build_env
    
    build_dag_engine
    build_cpp_components
    build_agent_sdk
    build_typescript_sdk
    build_benchmarks
    
    if [[ "${SKIP_INSTALL:-}" != "true" ]]; then
        install_components
    fi
    
    if [[ "${GENERATE_DOCS:-}" == "true" ]]; then
        generate_docs
    fi
    
    if [[ "${CREATE_PACKAGE:-}" == "true" ]]; then
        create_package
    fi
    
    log_success "ALT-LEDGER 2030 build completed successfully!"
    log_info "Build artifacts available in: $BUILD_DIR"
    
    # Print build summary
    echo
    echo "=== Build Summary ==="
    echo "Build Type: $BUILD_TYPE"
    echo "Build Phase: $BUILD_PHASE"
    echo "Tests: $([[ "$ENABLE_TESTS" == "ON" ]] && echo "Enabled" || echo "Disabled")"
    echo "Benchmarks: $([[ "$ENABLE_BENCHMARKS" == "ON" ]] && echo "Enabled" || echo "Disabled")"
    echo "Build Time: $(date)"
    echo "===================="
}

# Handle command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --phase)
            BUILD_PHASE="$2"
            shift 2
            ;;
        --type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --jobs)
            BUILD_JOBS="$2"
            shift 2
            ;;
        --no-tests)
            ENABLE_TESTS="OFF"
            shift
            ;;
        --no-benchmarks)
            ENABLE_BENCHMARKS="OFF"
            shift
            ;;
        --skip-install)
            SKIP_INSTALL="true"
            shift
            ;;
        --generate-docs)
            GENERATE_DOCS="true"
            shift
            ;;
        --create-package)
            CREATE_PACKAGE="true"
            shift
            ;;
        --help)
            cat << EOF
ALT-LEDGER 2030 Build Script

Usage: $0 [options]

Options:
  --phase <1|2|3>      Build phase (default: 1)
  --type <Debug|Release>  Build type (default: Release)
  --jobs <N>           Number of parallel jobs (default: nproc)
  --no-tests           Skip running tests
  --no-benchmarks      Skip building benchmarks
  --skip-install       Skip installation step
  --generate-docs      Generate documentation
  --create-package     Create distribution package
  --help               Show this help message

Environment Variables:
  BUILD_TYPE           Override build type
  BUILD_JOBS           Override number of jobs
  BUILD_PHASE          Override build phase
  ENABLE_TESTS         Enable/disable tests (ON/OFF)
  ENABLE_BENCHMARKS    Enable/disable benchmarks (ON/OFF)

Examples:
  $0                   # Basic build
  $0 --phase 2         # Build Phase 2 components
  $0 --type Debug      # Debug build
  $0 --no-tests        # Skip tests
  $0 --create-package  # Create distribution package
EOF
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Run main build process
main