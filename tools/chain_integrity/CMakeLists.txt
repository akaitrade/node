cmake_minimum_required(VERSION 3.12)

project(chain_integrity)

add_executable(chain_integrity src/main.cpp)

set (Boost_USE_MULTITHREADED ON)
set (Boost_USE_STATIC_LIBS ON)
if(NOT APPLE)
  set (Boost_USE_STATIC_RUNTIME ON)
endif()

find_package(Boost REQUIRED COMPONENTS system filesystem)

target_include_directories(chain_integrity PRIVATE
  ${CMAKE_SOURCE_DIR}/csdb/include
  ${CMAKE_SOURCE_DIR}/csnode/include
  ${CMAKE_SOURCE_DIR}/lib/include
  ${CMAKE_SOURCE_DIR}/client/include
  ${Boost_INCLUDE_DIR}
)

target_link_libraries(chain_integrity
  csnode
  csdb
  Boost::system
  Boost::filesystem
  Boost::disable_autolinking
)

if(WIN32)
  target_link_libraries(chain_integrity ws2_32 iphlpapi)
endif()

# Inherit backend defines from csdb so we know which backends to compile in.
if(CSDB_BACKEND STREQUAL "rocksdb" OR CSDB_BACKEND STREQUAL "both")
  target_compile_definitions(chain_integrity PRIVATE CSDB_USE_ROCKSDB)
endif()
if(CSDB_BACKEND STREQUAL "berkeleydb" OR CSDB_BACKEND STREQUAL "both")
  target_compile_definitions(chain_integrity PRIVATE CSDB_USE_BERKELEYDB)
endif()

set_property(TARGET chain_integrity PROPERTY CXX_STANDARD 17)
set_property(TARGET chain_integrity PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
