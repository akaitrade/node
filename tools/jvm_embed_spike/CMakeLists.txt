cmake_minimum_required(VERSION 3.12)

project(jvm_embed_spike LANGUAGES CXX)

# We need only the JNI headers. libjvm itself is loaded dynamically at runtime
# via LoadLibrary/dlopen; not link-time. find_package(JNI) populates
# JNI_INCLUDE_DIRS which is exactly what we want.
find_package(JNI REQUIRED)
message(STATUS "JNI include dirs: ${JNI_INCLUDE_DIRS}")

# Two ways this directory gets pulled in:
#  1. BUILD_JVM_EMBED_SPIKE alone -> standalone validation tool.
#  2. USE_EMBEDDED_JVM_NODE       -> production node links jvm_embed library
#     and needs EmbeddedExecutorBridge.class staged alongside node.exe at
#     runtime so cs::Executor can invoke methods on it via JNI.
# Either way the build is the same — both spike binary and library.

add_library(jvm_embed STATIC
    src/jvm_embed.cpp
    include/jvm_embed/jvm_embed.hpp
)

target_include_directories(jvm_embed
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${JNI_INCLUDE_DIRS}
)

set_property(TARGET jvm_embed PROPERTY CXX_STANDARD 17)
set_property(TARGET jvm_embed PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(UNIX AND NOT APPLE)
    target_link_libraries(jvm_embed PRIVATE dl)
endif()

# ----------------------------------------------------------------------------
# Compile the spike's Java side via javac. Output goes next to the executable
# under a "java/" directory so the default CWD-relative classpath works.
# ----------------------------------------------------------------------------
find_package(Java COMPONENTS Development)
if(NOT Java_JAVAC_EXECUTABLE)
    message(FATAL_ERROR
        "javac not found. The spike needs a JDK to compile HelloEmbed.java. "
        "Set JAVA_HOME or pass -DJava_JAVAC_EXECUTABLE=/path/to/javac.")
endif()
message(STATUS "javac: ${Java_JAVAC_EXECUTABLE}")

set(_class_out_dir "${CMAKE_CURRENT_BINARY_DIR}/java")
set(_java_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/java/HelloEmbed.java"
    "${CMAKE_CURRENT_SOURCE_DIR}/java/StubExecutor.java"
)
set(_bridge_src "${CMAKE_CURRENT_SOURCE_DIR}/java/EmbeddedExecutorBridge.java")
set(_stamp_file "${_class_out_dir}/.javac.stamp")

# Path to the real CREDITS contract-executor.jar. If present, we compile
# EmbeddedExecutorBridge against it so the spike can call into the actual
# executor implementation, not just the stub. Default points at the user's
# csnode_Default install; override on the cmake command line if needed.
set(JVM_EMBED_EXECUTOR_JAR "C:/csnode_Default/contract-executor.jar"
    CACHE FILEPATH "Path to contract-executor.jar for the JVM-embed bridge")
# The directory containing settings.properties (loaded as a classpath
# resource by ApplicationProperties).
set(JVM_EMBED_EXECUTOR_INSTALL_DIR "C:/csnode_Default"
    CACHE PATH "Directory containing settings.properties for the executor")
# sc-api-support.jar — needed so the executor's compile path can resolve
# com.credits.scapi.* classes when compiling a user contract.
set(JVM_EMBED_SCAPI_JAR "C:/csnode_Default/sc-api-support.jar"
    CACHE FILEPATH "Path to sc-api-support.jar")
set(_bridge_compiled FALSE)
if(EXISTS "${JVM_EMBED_EXECUTOR_JAR}")
    list(APPEND _java_sources "${_bridge_src}")
    set(_javac_classpath "-cp" "${JVM_EMBED_EXECUTOR_JAR}")
    set(_bridge_compiled TRUE)
    message(STATUS "jvm_embed_spike: building EmbeddedExecutorBridge against ${JVM_EMBED_EXECUTOR_JAR}")
else()
    set(_javac_classpath "")
    message(STATUS "jvm_embed_spike: contract-executor.jar not found at ${JVM_EMBED_EXECUTOR_JAR} — bridge not compiled (stub-only)")
endif()

# --release 11 keeps the bytecode compatible with the JDK we load at runtime
# (jvm.dll is JDK 11). Without this, a newer javac (e.g. Android Studio JBR's)
# emits class file version 61+ which JDK 11 refuses with UnsupportedClassVersionError.
add_custom_command(
    OUTPUT  "${_stamp_file}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_class_out_dir}"
    COMMAND ${Java_JAVAC_EXECUTABLE} --release 11 ${_javac_classpath} -d "${_class_out_dir}" ${_java_sources}
    COMMAND ${CMAKE_COMMAND} -E touch "${_stamp_file}"
    DEPENDS ${_java_sources}
    VERBATIM
    COMMENT "Compiling Java sources (--release 11)"
)

add_custom_target(jvm_embed_spike_java ALL DEPENDS "${_stamp_file}")

# ----------------------------------------------------------------------------
# The actual smoke-test executable.
# ----------------------------------------------------------------------------
add_executable(jvm_embed_spike src/main.cpp)
target_link_libraries(jvm_embed_spike PRIVATE jvm_embed)
add_dependencies(jvm_embed_spike jvm_embed_spike_java)

set_property(TARGET jvm_embed_spike PROPERTY CXX_STANDARD 17)
set_property(TARGET jvm_embed_spike PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(_bridge_compiled)
    target_compile_definitions(jvm_embed_spike PRIVATE
        JVM_EMBED_EXECUTOR_JAR="${JVM_EMBED_EXECUTOR_JAR}"
        JVM_EMBED_EXECUTOR_INSTALL_DIR="${JVM_EMBED_EXECUTOR_INSTALL_DIR}"
        JVM_EMBED_BRIDGE_AVAILABLE=1)
    if(EXISTS "${JVM_EMBED_SCAPI_JAR}")
        target_compile_definitions(jvm_embed_spike PRIVATE
            JVM_EMBED_SCAPI_JAR="${JVM_EMBED_SCAPI_JAR}")
    endif()
endif()

# Stage the compiled .class next to the binary so running with no args works.
add_custom_command(TARGET jvm_embed_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_class_out_dir}"
            "$<TARGET_FILE_DIR:jvm_embed_spike>/java"
)
