cmake_minimum_required(VERSION 3.1)

project(csconnector)

# Find required packages for WebSocket
find_package(Threads REQUIRED)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost REQUIRED COMPONENTS system thread)

# WebSocket++ configuration
# Using Boost.Asio instead of standalone ASIO since Boost is already a dependency
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASIO_STANDALONE")

# turn off optimization for this project in RelWithDebInfo only
# configure_msvc_no_optimization()

add_subdirectory(api_gen)
add_subdirectory(executor_gen)
add_subdirectory(variant_gen)

# It's not recommended to use file(GLOB, because it's called only in the
# Cmake-generator stage. When adding the files in the dir it won't be called - and the list of files
# will not be updated.
add_library(csconnector
    include/csstats.hpp
    src/csstats.cpp
    include/csconnector/csconnector.hpp
    src/csconnector.cpp
    src/apihandler.cpp
    include/apihandler.hpp
    include/debuglog.hpp
    include/tokens.hpp
    src/tokens.cpp
    include/profiler/profilerprocessor.hpp
    src/profilerprocessor.cpp
    include/profiler/profilereventhandler.hpp
    include/profiler/profiler.hpp
    src/profiler.cpp
    include/executormanager.hpp
    src/executormanager.cpp
    include/dumbcv.hpp
    src/dumbcv.cpp
    include/executor.hpp
    src/executor.cpp
    include/serializer.hpp
    include/apidiaghandler.hpp
    src/apidiaghandler.cpp
    include/websocket/websocketserver.hpp
    src/websocketserver.cpp
    include/websocket/websockethandler.hpp
    src/websockethandler.cpp
)

target_link_libraries (csconnector PUBLIC 
    csdb 
    csnode 
    lib 
    csconnector_gen 
    csconnector_executor_gen 
    variant_gen
    Boost::system
    Boost::thread
    Threads::Threads
    websocketpp::websocketpp
    nlohmann_json::nlohmann_json
)

# INCLUDE DIRECTORIES it's better not to set globally but for each project.
# INCLUDE DIRECTORIES consists of include libraries (in this case thrift and csdb)
# and should not be set. Those are included into INTERFACE libs and will be included automatically
# in target_link_libraries
target_include_directories(csconnector
  PUBLIC include
  PRIVATE src
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third-party/websocketpp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third-party/json/single_include
)

configure_msvc_flags()

# Add compile definitions
target_compile_definitions(csconnector PUBLIC 
    BINARY_TCP_API
    AJAX_IFACE
    NODE_API
    BINARY_TCP_EXECAPI
    WEBSOCKET_API
)

set_property(TARGET csconnector PROPERTY CXX_STANDARD 17)
set_property(TARGET csconnector PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
