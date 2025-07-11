cmake_minimum_required(VERSION 3.15)
project(credits-alt-ledger-2030 VERSION 1.0.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific definitions
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Windows 10
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
    add_compile_definitions(NOMINMAX)
    
    # Set static runtime to match Boost libraries
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # Alternative method for older CMake versions
    if(CMAKE_VERSION VERSION_LESS "3.15")
        foreach(flag_var
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
            if(${flag_var} MATCHES "/MD")
                string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
            endif()
        endforeach()
    endif()
endif()

# Build options
option(BUILD_PHASE1 "Build Phase 1 components" ON)
option(BUILD_PHASE2 "Build Phase 2 components" OFF)
option(BUILD_PHASE3 "Build Phase 3 components" OFF)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(ENABLE_INTEGRATION "Enable integration with existing CREDITS node" ON)

# Configure Boost options for Windows
if(WIN32)
    # Try multiple Boost configurations for Windows compatibility
    set(Boost_USE_STATIC_LIBS ON)
    set(Boost_USE_MULTITHREADED ON)
    
    # First try with static runtime (matches available libraries)
    set(Boost_USE_STATIC_RUNTIME ON)
    find_package(Boost 1.70 COMPONENTS system filesystem thread)
    
    # If not found, try with dynamic runtime
    if(NOT Boost_FOUND)
        message(STATUS "Trying Boost with dynamic runtime...")
        set(Boost_USE_STATIC_RUNTIME OFF)
        find_package(Boost 1.70 COMPONENTS system filesystem thread)
    endif()
    
    # If still not found, try dynamic libraries
    if(NOT Boost_FOUND)
        message(STATUS "Trying Boost with dynamic libraries...")
        set(Boost_USE_STATIC_LIBS OFF)
        set(Boost_USE_STATIC_RUNTIME OFF)
        find_package(Boost 1.70 COMPONENTS system filesystem thread)
    endif()
    
    # If still not found, make it required and fail
    if(NOT Boost_FOUND)
        find_package(Boost 1.70 REQUIRED COMPONENTS system filesystem thread)
    endif()
else()
    # Unix/Linux default configuration
    set(Boost_USE_STATIC_LIBS OFF)
    set(Boost_USE_MULTITHREADED ON)
    find_package(Boost 1.70 REQUIRED COMPONENTS system filesystem thread)
endif()

# Create modern CMake targets if they don't exist
if(Boost_FOUND AND NOT TARGET Boost::system)
    add_library(Boost::system INTERFACE IMPORTED)
    target_link_libraries(Boost::system INTERFACE ${Boost_SYSTEM_LIBRARY})
    target_include_directories(Boost::system INTERFACE ${Boost_INCLUDE_DIRS})
endif()

if(Boost_FOUND AND NOT TARGET Boost::filesystem)
    add_library(Boost::filesystem INTERFACE IMPORTED)
    target_link_libraries(Boost::filesystem INTERFACE ${Boost_FILESYSTEM_LIBRARY})
    target_include_directories(Boost::filesystem INTERFACE ${Boost_INCLUDE_DIRS})
endif()

if(Boost_FOUND AND NOT TARGET Boost::thread)
    add_library(Boost::thread INTERFACE IMPORTED)
    target_link_libraries(Boost::thread INTERFACE ${Boost_THREAD_LIBRARY})
    target_include_directories(Boost::thread INTERFACE ${Boost_INCLUDE_DIRS})
endif()

# Find optional PkgConfig (not available on all Windows systems)
find_package(PkgConfig QUIET)

# Find Rust for DAG engine
find_program(CARGO_EXECUTABLE cargo REQUIRED)

# Find optional packages
if(PKG_CONFIG_FOUND)
    pkg_check_modules(QUICHE QUIET quiche)
    if(NOT QUICHE_FOUND)
        message(WARNING "QUICHE not found, using fallback UDP implementation")
    endif()
else()
    message(WARNING "PkgConfig not found, skipping QUICHE detection - using fallback UDP implementation")
    set(QUICHE_FOUND FALSE)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/target/include  # Generated FFI headers
    ${CMAKE_CURRENT_SOURCE_DIR}/consensus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/networking/include
    ${CMAKE_CURRENT_SOURCE_DIR}/contracts/include
    ${CMAKE_CURRENT_SOURCE_DIR}/sharding/include
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/include
)


# Add subdirectories (only if they exist)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
    add_subdirectory(core)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/consensus/CMakeLists.txt")
    add_subdirectory(consensus)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/networking/CMakeLists.txt")
    add_subdirectory(networking)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/contracts/CMakeLists.txt")
    add_subdirectory(contracts)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sharding/CMakeLists.txt")
    add_subdirectory(sharding)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/CMakeLists.txt")
    add_subdirectory(integration)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/migration/CMakeLists.txt")
    add_subdirectory(migration)
endif()

if(BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
endif()

# Create combined library (only with available components)
set(LIBRARY_SOURCES "")

# Add module objects if they exist
if(TARGET consensus_module)
    list(APPEND LIBRARY_SOURCES $<TARGET_OBJECTS:consensus_module>)
endif()

if(TARGET networking_module)
    list(APPEND LIBRARY_SOURCES $<TARGET_OBJECTS:networking_module>)
endif()

if(TARGET contracts_module)
    list(APPEND LIBRARY_SOURCES $<TARGET_OBJECTS:contracts_module>)
endif()

if(TARGET sharding_module)
    list(APPEND LIBRARY_SOURCES $<TARGET_OBJECTS:sharding_module>)
endif()

if(TARGET integration_module)
    list(APPEND LIBRARY_SOURCES $<TARGET_OBJECTS:integration_module>)
endif()

# Only create the library if we have sources, otherwise just use the Rust library
if(LIBRARY_SOURCES)
    add_library(credits_alt_ledger SHARED ${LIBRARY_SOURCES})
    
    # Ensure Rust library is built first
    if(TARGET rust_dag_engine)
        add_dependencies(credits_alt_ledger rust_dag_engine)
    endif()
    
    target_link_libraries(credits_alt_ledger
        ${DAG_ENGINE_LIB}
        Boost::system
        Boost::filesystem
        Boost::thread
    )
    
    # Add platform-specific libraries
    if(NOT WIN32)
        target_link_libraries(credits_alt_ledger pthread dl)
    else()
        target_link_libraries(credits_alt_ledger ws2_32 userenv)
    endif()
else()
    # Fallback: create a simple interface library
    add_library(credits_alt_ledger INTERFACE)
    
    # Ensure Rust library is built first
    if(TARGET rust_dag_engine)
        add_dependencies(credits_alt_ledger rust_dag_engine)
    endif()
    
    target_link_libraries(credits_alt_ledger INTERFACE
        ${DAG_ENGINE_LIB}
        Boost::system
        Boost::filesystem
        Boost::thread
    )
endif()

# Install targets
install(TARGETS credits_alt_ledger
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY core/include/ DESTINATION include/credits_alt_ledger)
install(DIRECTORY consensus/include/ DESTINATION include/credits_alt_ledger)
install(DIRECTORY networking/include/ DESTINATION include/credits_alt_ledger)
install(DIRECTORY contracts/include/ DESTINATION include/credits_alt_ledger)
install(DIRECTORY sharding/include/ DESTINATION include/credits_alt_ledger)
install(DIRECTORY integration/include/ DESTINATION include/credits_alt_ledger)