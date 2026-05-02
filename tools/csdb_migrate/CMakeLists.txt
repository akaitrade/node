cmake_minimum_required(VERSION 3.12)

project(csdb_migrate)

add_executable(csdb_migrate src/main.cpp)

set (Boost_USE_MULTITHREADED ON)
set (Boost_USE_STATIC_LIBS ON)
if(NOT APPLE)
  set (Boost_USE_STATIC_RUNTIME ON)
endif()

find_package(Boost REQUIRED COMPONENTS system filesystem)

target_include_directories(csdb_migrate PRIVATE
  ${CMAKE_SOURCE_DIR}/csdb/include
  ${CMAKE_SOURCE_DIR}/lib/include
  ${CMAKE_SOURCE_DIR}/client/include
  ${Boost_INCLUDE_DIR}
)

target_link_libraries(csdb_migrate
  csdb
  Boost::system
  Boost::filesystem
  Boost::disable_autolinking
)

if(WIN32)
  # BerkeleyDB on Windows references Winsock (replication manager); link it.
  target_link_libraries(csdb_migrate ws2_32 iphlpapi)
endif()

set_property(TARGET csdb_migrate PROPERTY CXX_STANDARD 17)
set_property(TARGET csdb_migrate PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
