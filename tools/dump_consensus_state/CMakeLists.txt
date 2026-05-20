cmake_minimum_required(VERSION 3.12)

project(dump_consensus_state)

add_executable(dump_consensus_state src/main.cpp)

set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_LIBS ON)
if(NOT APPLE)
  set(Boost_USE_STATIC_RUNTIME ON)
endif()

find_package(Boost REQUIRED COMPONENTS system filesystem)

target_include_directories(dump_consensus_state PRIVATE
  ${CMAKE_SOURCE_DIR}/csdb/include
  ${CMAKE_SOURCE_DIR}/csnode/include
  ${CMAKE_SOURCE_DIR}/solver/include
  ${CMAKE_SOURCE_DIR}/lib/include
  ${CMAKE_SOURCE_DIR}/client/include
  ${Boost_INCLUDE_DIR}
)

target_link_libraries(dump_consensus_state
  csdb
  solver
  Boost::system
  Boost::filesystem
  Boost::disable_autolinking
)

if(WIN32)
  target_link_libraries(dump_consensus_state ws2_32 iphlpapi)
endif()

set_property(TARGET dump_consensus_state PROPERTY CXX_STANDARD 17)
set_property(TARGET dump_consensus_state PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
