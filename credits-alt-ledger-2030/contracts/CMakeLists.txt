cmake_minimum_required(VERSION 3.15)

# Find Wasmtime
find_path(WASMTIME_INCLUDE_DIR wasmtime.h)
find_library(WASMTIME_LIBRARY wasmtime)

if(NOT WASMTIME_INCLUDE_DIR OR NOT WASMTIME_LIBRARY)
    message(WARNING "Wasmtime not found, WASM runtime will be disabled")
    set(ENABLE_WASM_RUNTIME OFF)
else()
    set(ENABLE_WASM_RUNTIME ON)
    message(STATUS "Found Wasmtime: ${WASMTIME_LIBRARY}")
endif()

# Collect available source files
set(CONTRACTS_SOURCES "")

# Check for each source file and add if exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/wasm_executor.cpp")
    list(APPEND CONTRACTS_SOURCES src/wasm_executor.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cns_host_functions.cpp")
    list(APPEND CONTRACTS_SOURCES src/cns_host_functions.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/contract_manager.cpp")
    list(APPEND CONTRACTS_SOURCES src/contract_manager.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gas_metering.cpp")
    list(APPEND CONTRACTS_SOURCES src/gas_metering.cpp)
endif()

# Create contracts module only if we have sources
if(CONTRACTS_SOURCES)
    add_library(contracts_module OBJECT ${CONTRACTS_SOURCES})
    # Ensure DAG engine is built first (for generated headers)
    add_dependencies(contracts_module rust_dag_engine)
else()
    # Create empty object library
    add_library(contracts_module OBJECT)
    target_sources(contracts_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
endif()

target_include_directories(contracts_module PUBLIC
    include
    ${Boost_INCLUDE_DIRS}
)

if(ENABLE_WASM_RUNTIME)
    target_compile_definitions(contracts_module PRIVATE ENABLE_WASM_RUNTIME)
    target_include_directories(contracts_module PRIVATE ${WASMTIME_INCLUDE_DIR})
endif()

target_link_libraries(contracts_module
    ${Boost_LIBRARIES}
)

if(ENABLE_WASM_RUNTIME)
    target_link_libraries(contracts_module ${WASMTIME_LIBRARY})
endif()