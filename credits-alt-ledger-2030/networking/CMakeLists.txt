cmake_minimum_required(VERSION 3.15)

# Find QUIC library (optional)
pkg_check_modules(QUICHE QUIET quiche)

# Collect available source files
set(NETWORKING_SOURCES "")

# Check for each source file and add if exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ctdp_v2_transport.cpp")
    list(APPEND NETWORKING_SOURCES src/ctdp_v2_transport.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dag_frame_handler.cpp")
    list(APPEND NETWORKING_SOURCES src/dag_frame_handler.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gossip_overlay.cpp")
    list(APPEND NETWORKING_SOURCES src/gossip_overlay.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/network_coordinator.cpp")
    list(APPEND NETWORKING_SOURCES src/network_coordinator.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/quic_transport.cpp")
    list(APPEND NETWORKING_SOURCES src/quic_transport.cpp)
endif()

# Create networking module only if we have sources
if(NETWORKING_SOURCES)
    add_library(networking_module OBJECT ${NETWORKING_SOURCES})
    # Ensure DAG engine is built first (for generated headers)
    add_dependencies(networking_module rust_dag_engine)
else()
    # Create empty object library
    add_library(networking_module OBJECT)
    target_sources(networking_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
endif()

target_include_directories(networking_module PUBLIC
    include
    ${Boost_INCLUDE_DIRS}
)

if(QUICHE_FOUND)
    target_compile_definitions(networking_module PRIVATE HAVE_QUICHE)
    target_include_directories(networking_module PRIVATE ${QUICHE_INCLUDE_DIRS})
endif()

target_link_libraries(networking_module
    ${Boost_LIBRARIES}
)

if(QUICHE_FOUND)
    target_link_libraries(networking_module ${QUICHE_LIBRARIES})
endif()