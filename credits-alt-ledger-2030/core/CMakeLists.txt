cmake_minimum_required(VERSION 3.15)

# Build Rust DAG engine
if(WIN32)
    add_custom_target(rust_dag_engine
        COMMAND ${CARGO_EXECUTABLE} build --release --features sled-backend
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building Rust DAG engine (Windows/Sled backend)"
        BYPRODUCTS
            ${CMAKE_CURRENT_SOURCE_DIR}/target/release/dag_engine.lib
            ${CMAKE_CURRENT_SOURCE_DIR}/target/include/dag_engine.h
    )
    
    # Create imported library for linking
    add_library(dag_engine_rust STATIC IMPORTED)
    set_target_properties(dag_engine_rust PROPERTIES
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/target/release/dag_engine.lib
    )
else()
    add_custom_target(rust_dag_engine
        COMMAND ${CARGO_EXECUTABLE} build --release --features rocksdb-backend
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building Rust DAG engine (Unix/RocksDB backend)"
        BYPRODUCTS
            ${CMAKE_CURRENT_SOURCE_DIR}/target/release/libdag_engine.a
            ${CMAKE_CURRENT_SOURCE_DIR}/target/include/dag_engine.h
    )
    
    # Create imported library for linking
    add_library(dag_engine_rust STATIC IMPORTED)
    set_target_properties(dag_engine_rust PROPERTIES
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/target/release/libdag_engine.a
    )
endif()

# Include directory for generated headers
set(DAG_ENGINE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/target/include)

# Export variables for parent CMakeLists
if(WIN32)
    set(DAG_ENGINE_LIB ${CMAKE_CURRENT_SOURCE_DIR}/target/release/dag_engine.lib PARENT_SCOPE)
else()
    set(DAG_ENGINE_LIB ${CMAKE_CURRENT_SOURCE_DIR}/target/release/libdag_engine.a PARENT_SCOPE)
endif()
set(DAG_ENGINE_INCLUDE_DIR ${DAG_ENGINE_INCLUDE_DIR} PARENT_SCOPE)