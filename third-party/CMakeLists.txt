cmake_minimum_required(VERSION 3.10)

# default /W3 or /W4 for msvc at googletest
add_subdirectory(googletest)
add_subdirectory(thrift)

configure_third_party_compiler_flags()

add_subdirectory(berkeleydb)
add_subdirectory(lz4)
add_subdirectory(rang)
add_subdirectory(nameof)

# Header-only libraries
add_library(websocketpp INTERFACE)
target_include_directories(websocketpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/websocketpp)
add_library(websocketpp::websocketpp ALIAS websocketpp)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/json/single_include)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
