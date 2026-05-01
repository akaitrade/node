// Minimal JNI Invocation API spike for the Embed_JVM project.
// Loads libjvm dynamically (no link-time dependency), starts a JVM, and
// invokes static methods on a Java class. Single VM per process.
//
// Public surface intentionally tiny — this is a feasibility check, not
// the production wrapper that will replace the Thrift executor client.

#ifndef _CREDITS_JVM_EMBED_HPP_INCLUDED_
#define _CREDITS_JVM_EMBED_HPP_INCLUDED_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct JNIEnv_;
struct JavaVM_;
typedef JNIEnv_ JNIEnv;
typedef JavaVM_ JavaVM;

namespace jvm_embed {

// A pre-resolved static method handle. The class is held as a global ref so
// repeat calls skip FindClass + GetStaticMethodID. Dispose via Vm::release().
// Internally cls is a jclass (global ref), mid is a jmethodID — exposed as
// void* to avoid pulling jni.h into our public header.
struct StaticMethodHandle {
    void* cls = nullptr;        // jclass (GLOBAL ref while valid)
    void* mid = nullptr;        // jmethodID
    bool valid() const noexcept { return cls != nullptr && mid != nullptr; }
};

struct InitOptions {
    // One classpath entry per element. Each may be a directory of .class files
    // or a path to a .jar. The wrapper concatenates with the platform separator.
    std::vector<std::string> classpath;

    // Heap, stack, GC etc. Forwarded verbatim as JavaVMOption strings.
    std::vector<std::string> jvmOptions;

    // If empty, resolved from the JAVA_HOME environment variable.
    // Should point at the JDK root (the one containing bin/server/jvm.dll on
    // Windows or lib/server/libjvm.so on Linux).
    std::string javaHomeOverride;
};

class Vm {
public:
    Vm();
    ~Vm();

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Loads libjvm and creates the VM. Idempotent in the sense that one Vm
    // instance owns one JVM; HotSpot allows only one JVM per process for the
    // process lifetime, so creating a second Vm in the same process will fail.
    bool start(const InitOptions& opts);

    // Calls a static String method that takes no parameters. Returns nullopt
    // on any JNI failure (class not found, method not found, exception).
    std::optional<std::string> callStaticStringNoArg(const std::string& className,
                                                     const std::string& methodName);

    // Calls a static String method that takes one String parameter.
    std::optional<std::string> callStaticStringOneStringArg(const std::string& className,
                                                            const std::string& methodName,
                                                            const std::string& arg);

    // Calls a static int method (int, int) -> int.
    std::optional<int32_t> callStaticIntTwoInts(const std::string& className,
                                                const std::string& methodName,
                                                int32_t a, int32_t b);

    // Calls a static method `byte[] m(byte[])`. Used to validate byte-buffer
    // round-tripping for any future call that passes contract bytecode/state
    // (Thrift `binary` fields all map to byte[] on the Java side).
    std::optional<std::vector<uint8_t>>
    callStaticBytesOneBytesArg(const std::string& className,
                               const std::string& methodName,
                               const std::vector<uint8_t>& arg);

    // Resolve a static method once and reuse the handle. Avoids repeated
    // FindClass + GetStaticMethodID overhead for hot-path calls. The class
    // ref is held global; release() returns it.
    StaticMethodHandle resolveStaticMethod(const std::string& className,
                                           const std::string& methodName,
                                           const std::string& signature);
    void release(StaticMethodHandle& h);

    // Cached-handle variants of the call helpers.
    std::optional<int32_t> callStaticIntTwoIntsCached(const StaticMethodHandle& h,
                                                      int32_t a, int32_t b);

    // ----- Stub of the real ContractExecutor.getExecutorBuildVersion -------
    // Mirrors the Thrift result shape:
    //   ExecutorBuildVersionResult {
    //       APIResponse { i8 code; string message }
    //       i32 commitNumber
    //       string commitHash
    //   }
    // This is the first end-to-end JNI wrapper of a service-shaped method.
    // The same pattern (find class, find method, call, extract fields) is
    // what the production wrapper will use for executeByteCode etc.
    struct ExecutorBuildVersionResult {
        int8_t      code         = 0;
        std::string message;
        int32_t     commitNumber = 0;
        std::string commitHash;
    };

    std::optional<ExecutorBuildVersionResult>
    callGetExecutorBuildVersion(int16_t version);

    // Most recent error message produced by start() or any call. Empty when no
    // error has occurred since the last successful operation.
    const std::string& lastError() const noexcept;

    // True iff start() succeeded and the VM is currently usable.
    bool isStarted() const noexcept;

    // Returns the underlying JavaVM* for callers that want to attach their own
    // threads (the threadguard helper does this RAII-style — prefer that).
    JavaVM* javaVm() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// RAII helper: ensures the calling thread is attached to the JVM for the
// lifetime of the guard. If already attached, takes no action and detach is
// a no-op. If not attached, attaches as a daemon (so JVM shutdown does not
// wait for these threads to detach explicitly), and detaches on destruction.
//
// Usage:
//   ThreadGuard guard(vm);
//   if (!guard.ok()) return;
//   JNIEnv* env = guard.env();
//   ... use env ...
class ThreadGuard {
public:
    explicit ThreadGuard(Vm& vm);
    ~ThreadGuard();

    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;

    bool   ok() const noexcept   { return env_ != nullptr; }
    JNIEnv* env() const noexcept { return env_; }

private:
    JavaVM* vm_;
    JNIEnv* env_;
    bool    weAttached_;  // true iff this guard caused the attach (so it owns the detach)
};

}  // namespace jvm_embed

#endif  // _CREDITS_JVM_EMBED_HPP_INCLUDED_
