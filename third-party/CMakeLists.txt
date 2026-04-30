cmake_minimum_required(VERSION 3.10)

# default /W3 or /W4 for msvc at googletest
add_subdirectory(googletest)
add_subdirectory(thrift)

configure_third_party_compiler_flags()

# lz4 is needed by csdb regardless of backend, AND we hand it to the rocksdb
# build below as the LZ4 implementation (so rocksdb can use kLZ4Compression
# without us pulling in a second copy of the lz4 sources).
add_subdirectory(lz4)

if(CSDB_BACKEND STREQUAL "berkeleydb")
    add_subdirectory(berkeleydb)
endif()

if(CSDB_BACKEND STREQUAL "rocksdb")
    set(WITH_TESTS           OFF CACHE BOOL   "" FORCE)
    set(WITH_TOOLS           OFF CACHE BOOL   "" FORCE)
    set(WITH_CORE_TOOLS      OFF CACHE BOOL   "" FORCE)
    set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL   "" FORCE)
    set(WITH_ALL_TESTS       OFF CACHE BOOL   "" FORCE)
    set(WITH_TRACE_TOOLS     OFF CACHE BOOL   "" FORCE)
    set(WITH_GFLAGS          OFF CACHE BOOL   "" FORCE)
    set(WITH_JNI             OFF CACHE BOOL   "" FORCE)
    set(WITH_EXAMPLES        OFF CACHE BOOL   "" FORCE)
    set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL   "" FORCE)
    set(FAIL_ON_WARNINGS     OFF CACHE BOOL   "" FORCE)
    set(USE_RTTI             ON  CACHE BOOL   "" FORCE)
    set(WITH_MD_LIBRARY      OFF CACHE BOOL   "" FORCE)  # /MT static runtime to match the rest of the project
    set(PORTABLE             "1" CACHE STRING "" FORCE)  # baseline x86-64; no -mavx2 etc.

    # Wire our internal lz4 target into rocksdb's WITH_LZ4 path.
    #
    # On MSVC rocksdb uses third-party/rocksdb/thirdparty.inc (NOT the find_package
    # path used on non-MSVC). thirdparty.inc unconditionally hardcodes LZ4_INCLUDE
    # to a bogus $ENV{THIRDPARTY_HOME}/LZ4.Library/... path, then conditionally
    # overrides it from env vars LZ4_INCLUDE/LZ4_LIB_DEBUG/LZ4_LIB_RELEASE if they
    # are set. Setting them as plain CMake variables doesn't work -- the
    # `set(LZ4_INCLUDE ${LZ4_HOME}/...)` call in thirdparty.inc clobbers them.
    # Use the env-var override instead. LZ4_LIB_* set to the literal string "lz4"
    # makes thirdparty.inc emit our `lz4` target name into the link line; CMake
    # resolves it to the target defined by add_subdirectory(lz4) above.
    set(WITH_LZ4 ON CACHE BOOL "" FORCE)
    set(ENV{LZ4_INCLUDE}     "${CMAKE_CURRENT_SOURCE_DIR}/lz4")
    set(ENV{LZ4_LIB_DEBUG}   "lz4")
    set(ENV{LZ4_LIB_RELEASE} "lz4")

    # rocksdb's port/win/env_win.cc uses FILE_ID_INFO and other Win8+ structs.
    # The root CMakeLists pins _WIN32_WINNT=0x0601 (Win7); shadow that for the
    # rocksdb subtree only, then restore so siblings (rang, nameof) still see
    # the original.
    if(WIN32)
        remove_definitions(-D_WIN32_WINNT=0x0601)
        add_definitions(-D_WIN32_WINNT=0x0A00)  # _WIN32_WINNT_WIN10
    endif()
    add_subdirectory(rocksdb)
    if(WIN32)
        remove_definitions(-D_WIN32_WINNT=0x0A00)
        add_definitions(-D_WIN32_WINNT=0x0601)
    endif()
endif()

add_subdirectory(rang)
add_subdirectory(nameof)
