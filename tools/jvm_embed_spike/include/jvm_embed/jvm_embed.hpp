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

    // The Java side must expose:
    //   public static <ClassName>$BuildVersionResult getExecutorBuildVersion(short version)
    // where BuildVersionResult has public final fields:
    //   byte code; String message; int commitNumber; String commitHash
    // Both StubExecutor (test) and EmbeddedExecutorBridge (real) match this shape.
    std::optional<ExecutorBuildVersionResult>
    callGetExecutorBuildVersion(int16_t version,
                                const std::string& className = "StubExecutor");

    // ----- compileSourceCode wrapper -----------------------------------
    // Mirrors the Thrift CompileSourceCodeResult flattened into a struct
    // with parallel arrays (one entry per output class). Targets
    // EmbeddedExecutorBridge.compileSourceCode in Java.
    struct CompiledClass {
        std::string          name;
        std::vector<uint8_t> byteCode;
    };
    struct CompileSourceCodeResult {
        int8_t                     code = 0;
        std::string                message;
        std::vector<CompiledClass> classes;
    };

    std::optional<CompileSourceCodeResult>
    callCompileSourceCode(const std::string& source, int16_t version);

    // ----- getContractMethods wrapper ----------------------------------
    // Input: parallel arrays — one entry per class file (name + bytecode).
    // Output: flattened method introspection results.
    struct AnnotationInfo {
        std::string name;
        std::vector<std::pair<std::string, std::string>> arguments;
    };
    struct MethodArgumentInfo {
        std::string                  type;
        std::string                  name;
        std::vector<AnnotationInfo>  annotations;
    };
    struct MethodInfo {
        std::string                       name;
        std::string                       returnType;
        std::vector<MethodArgumentInfo>   arguments;
        std::vector<AnnotationInfo>       annotations;
    };
    struct GetContractMethodsResult {
        int8_t                  code = 0;
        std::string             message;
        std::vector<MethodInfo> methods;
    };

    std::optional<GetContractMethodsResult>
    callGetContractMethods(const std::vector<std::string>&             classNames,
                           const std::vector<std::vector<uint8_t>>&    byteCodes,
                           int16_t                                     version);

    // ----- Variant tagged-value (mirrors EmbeddedExecutorBridge.TaggedVariant)
    // The Java side flattens Thrift's 25-case Variant union to one of six
    // tagged shapes. Recursive cases (V_LIST, V_MAP, V_OBJECT etc.) come
    // back as VTAG_OTHER with their toString() in `repr` — sufficient for
    // most contracts; production users that need structured access to
    // collection-shaped variants can extend the bridge later.
    enum VariantTag : uint8_t {
        VTAG_NULL          = 0,
        VTAG_BOOL          = 1,
        VTAG_LONG          = 2,
        VTAG_DOUBLE        = 3,
        VTAG_STRING        = 4,
        VTAG_BYTES         = 5,
        VTAG_OTHER         = 6,
        VTAG_VOID          = 7,
        VTAG_AMOUNT        = 8,
        VTAG_THRIFT_BINARY = 9,   // recursive Variant cases via TBinaryProtocol blob
    };
    struct Variant {
        VariantTag           tag = VTAG_NULL;
        bool                 boolVal   = false;
        int64_t              longVal   = 0;
        double               doubleVal = 0.0;
        std::string          stringVal;
        std::vector<uint8_t> bytesVal;
        std::string          repr;
    };

    // ----- getContractVariables wrapper --------------------------------
    struct GetContractVariablesResult {
        int8_t                              code = 0;
        std::string                         message;
        std::vector<std::pair<std::string, Variant>> variables;
    };

    std::optional<GetContractVariablesResult>
    callGetContractVariables(const std::vector<std::string>&          classNames,
                             const std::vector<std::vector<uint8_t>>& byteCodes,
                             const std::vector<uint8_t>&              state,
                             int16_t                                  version);

    // Synthetic round-trip of Variants of every tag. Used to validate the
    // unwrapping pipeline without requiring real contract state.
    std::optional<std::vector<Variant>> callMakeVariantSamples();

    // Variant marshalling self-test. Constructs every Thrift Variant case
    // on the Java side, flattens it, and reports the actual tag against
    // the expected tag. Run at startup to catch bugs (missing flatten case,
    // stale tag clamp, marshalling errors) before they reach a live chain.
    struct VariantSelfTestCase {
        std::string name;
        int32_t     expectedTag = 0;
        int32_t     actualTag   = 0;
        bool        ok          = false;
    };
    struct VariantSelfTestReport {
        std::vector<VariantSelfTestCase> cases;
        size_t passes  = 0;
        size_t fails   = 0;
        bool   allOk() const noexcept { return fails == 0 && !cases.empty(); }
    };
    std::optional<VariantSelfTestReport> runVariantSelfTest();

    // ----- executeByteCode wrapper -------------------------------------
    // Runs a single method on a freshly-deployed contract (or one with the
    // given instance state). Returns the first method's status, return
    // Variant, execution cost, and new contract state.
    struct ExecuteResult {
        int8_t               code           = 0;     // overall executor status
        std::string          message;
        int8_t               methodCode     = 0;     // first method's status
        std::string          methodMessage;
        Variant              retVal;
        int64_t              executionCost  = 0;
        std::vector<uint8_t> newState;
    };

    std::optional<ExecuteResult>
    callExecuteByteCode(int64_t                                  accessId,
                        const std::vector<uint8_t>&              initiatorAddress,
                        const std::vector<uint8_t>&              contractAddress,
                        const std::vector<std::string>&          classNames,
                        const std::vector<std::vector<uint8_t>>& byteCodes,
                        const std::vector<uint8_t>&              instance,
                        bool                                     stateCanModify,
                        const std::string&                       methodName,
                        int64_t                                  executionTimeoutMs,
                        int16_t                                  version);

    // ----- Full-fidelity executeByteCode -------------------------------
    // Returns the COMPLETE result: every SetterMethodResult, full
    // contractsState map (one entry per affected contract), and emitted
    // transaction list. cs::Executor uses this when wiring the Thrift
    // executeByteCode path through the embedded JVM — chain integrity
    // depends on no field being dropped.
    struct FullStateEntry {
        std::vector<uint8_t> address;
        std::vector<uint8_t> state;
    };
    struct FullEmittedTxn {
        std::vector<uint8_t> source;
        std::vector<uint8_t> target;
        int32_t              amountIntegral = 0;
        int64_t              amountFraction = 0;
        std::vector<uint8_t> userData;
    };
    struct FullSetterResult {
        int8_t                       code = 0;
        std::string                  message;
        Variant                      retVal;
        std::vector<FullStateEntry>  contractsState;
        std::vector<FullEmittedTxn>  emittedTransactions;
        int64_t                      executionCost = 0;
    };
    struct FullExecuteResult {
        int8_t                          code = 0;
        std::string                     message;
        std::vector<FullSetterResult>   results;
    };

    // Parallel arrays describing a single MethodHeader's params, in the
    // order they appear in the call. paramTags must have exactly one entry
    // per parameter; the other arrays are read at the same index when the
    // tag selects them. (Each call site fills one of bools/longs/doubles/
    // strings/bytes per param; the unused arrays may be empty.)
    struct VariantInputs {
        std::vector<int32_t>              tags;
        std::vector<bool>                 bools;
        std::vector<int64_t>              longs;
        std::vector<double>               doubles;
        std::vector<std::string>          strings;
        std::vector<std::vector<uint8_t>> bytes;
    };

    // Multi-method shape: methodNames.size() MethodHeaders, each with its
    // own param sublist sliced from paramsFlat using paramGroupSizes.
    // sum(paramGroupSizes) == paramsFlat.tags.size().
    std::optional<FullExecuteResult>
    callExecuteByteCodeFull(int64_t                                  accessId,
                            const std::vector<uint8_t>&              initiatorAddress,
                            const std::vector<uint8_t>&              contractAddress,
                            const std::vector<std::string>&          classNames,
                            const std::vector<std::vector<uint8_t>>& byteCodes,
                            const std::vector<uint8_t>&              instance,
                            bool                                     stateCanModify,
                            const std::vector<std::string>&          methodNames,
                            const std::vector<int32_t>&              paramGroupSizes,
                            const VariantInputs&                     paramsFlat,
                            int64_t                                  executionTimeoutMs,
                            int16_t                                  version);

    // Multi-call form: same method invoked N times with N param sets.
    // paramGroupSizes[i] = number of params in call i;
    // params (flat) is the concat of all calls' params, length =
    // sum(paramGroupSizes).
    std::optional<FullExecuteResult>
    callExecuteByteCodeMultipleFull(int64_t                                  accessId,
                                    const std::vector<uint8_t>&              initiatorAddress,
                                    const std::vector<uint8_t>&              contractAddress,
                                    const std::vector<std::string>&          classNames,
                                    const std::vector<std::vector<uint8_t>>& byteCodes,
                                    const std::vector<uint8_t>&              instance,
                                    bool                                     stateCanModify,
                                    const std::string&                       methodName,
                                    const std::vector<int32_t>&              paramGroupSizes,
                                    const VariantInputs&                     paramsFlat,
                                    int64_t                                  executionTimeoutMs,
                                    int16_t                                  version);

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
