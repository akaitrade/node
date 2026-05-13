cmake_minimum_required(VERSION 3.12)

project(state_audit)

set (Boost_USE_MULTITHREADED ON)
set (Boost_USE_STATIC_LIBS ON)
if(NOT APPLE)
  set (Boost_USE_STATIC_RUNTIME ON)
endif()

find_package(Boost REQUIRED COMPONENTS system filesystem)

add_executable(state_audit src/main.cpp)

target_include_directories(state_audit PRIVATE
  ${CMAKE_SOURCE_DIR}/csdb/include
  ${CMAKE_SOURCE_DIR}/csnode/include
  ${CMAKE_SOURCE_DIR}/lib/include
  ${CMAKE_SOURCE_DIR}/client/include
  ${CMAKE_SOURCE_DIR}/cscrypto/include
  ${CMAKE_SOURCE_DIR}/third-party/lmdbxx
  ${CMAKE_SOURCE_DIR}/third-party/lmdb/libraries/liblmdb
  ${Boost_INCLUDE_DIR}
)

target_link_libraries(state_audit
  csnode
  csdb
  lmdb
  Boost::system
  Boost::filesystem
  Boost::disable_autolinking
  # link p2p_network AFTER easylogging gets transitively included from csnode
  # so log.cc.o (which contains INITIALIZE_EASYLOGGINGPP defining elStorage)
  # is pulled in to satisfy the unresolved symbol.
  -Wl,--start-group csnode net p2p_network easylogging -Wl,--end-group
)

if(WIN32)
  target_link_libraries(state_audit ws2_32 iphlpapi)
endif()

if(CSDB_BACKEND STREQUAL "rocksdb" OR CSDB_BACKEND STREQUAL "both")
  target_compile_definitions(state_audit PRIVATE CSDB_USE_ROCKSDB)
endif()
if(CSDB_BACKEND STREQUAL "berkeleydb" OR CSDB_BACKEND STREQUAL "both")
  target_compile_definitions(state_audit PRIVATE CSDB_USE_BERKELEYDB)
endif()

set_property(TARGET state_audit PROPERTY CXX_STANDARD 17)
set_property(TARGET state_audit PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
