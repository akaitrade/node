cmake_minimum_required(VERSION 3.10)

project (lz4)

add_library(${PROJECT_NAME}
  lz4.h
  lz4.c
  lz4hc.h
  lz4hc.c )

# Expose the directory so consumers (rocksdb, csdb) get `-I third-party/lz4`
# automatically when they `target_link_libraries(... lz4 ...)`.
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
