// JNI Invocation API spike implementation.
//
// Loads jvm.dll (Windows) / libjvm.so (Linux) via LoadLibrary/dlopen so the
// binary doesn't bake a JDK path into its rpath / import table. The only two
// symbols resolved from libjvm are JNI_CreateJavaVM and JNI_GetDefaultJavaVMInitArgs;
// everything else flows through the JavaVM* / JNIEnv* function tables.

#include <jvm_embed/jvm_embed.hpp>

#include <cstdlib>
#include <jni.h>
#include <sstream>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static const char* kPathSep = ";";
  static const char* kJvmRelative = "\\bin\\server\\jvm.dll";
  using LibHandle = HMODULE;
  static LibHandle loadLib(const char* path) { return LoadLibraryA(path); }
  static void* loadSym(LibHandle h, const char* sym) { return reinterpret_cast<void*>(GetProcAddress(h, sym)); }
  static std::string lastSysError() {
      DWORD code = GetLastError();
      char buf[256] = {0};
      FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, code, 0, buf, sizeof(buf), nullptr);
      std::ostringstream os;
      os << "win32 error " << code << ": " << buf;
      return os.str();
  }
#else
  #include <dlfcn.h>
  static const char* kPathSep = ":";
  static const char* kJvmRelative = "/lib/server/libjvm.so";
  using LibHandle = void*;
  static LibHandle loadLib(const char* path) { return dlopen(path, RTLD_NOW | RTLD_GLOBAL); }
  static void* loadSym(LibHandle h, const char* sym) { return dlsym(h, sym); }
  static std::string lastSysError() {
      const char* e = dlerror();
      return e ? std::string(e) : std::string("(no dl error)");
  }
#endif

namespace jvm_embed {

namespace {
typedef jint (JNICALL* CreateJavaVMFn)(JavaVM**, void**, void*);

std::string joinClasspath(const std::vector<std::string>& entries) {
    std::ostringstream os;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) os << kPathSep;
        os << entries[i];
    }
    return os.str();
}

// Drains and clears any pending Java exception, returning a short description.
std::string drainPendingException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return {};
    jthrowable th = env->ExceptionOccurred();
    env->ExceptionClear();   // mandatory before further JNI calls

    if (!th) return "unknown exception";

    std::string out = "java exception";
    jclass thCls = env->GetObjectClass(th);
    if (thCls) {
        jmethodID toString = env->GetMethodID(thCls, "toString", "()Ljava/lang/String;");
        if (toString) {
            jobject msgObj = env->CallObjectMethod(th, toString);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
            } else if (msgObj) {
                jstring jstr = static_cast<jstring>(msgObj);
                const char* utf = env->GetStringUTFChars(jstr, nullptr);
                if (utf) {
                    out = utf;
                    env->ReleaseStringUTFChars(jstr, utf);
                }
                env->DeleteLocalRef(msgObj);
            }
        }
        env->DeleteLocalRef(thCls);
    }
    env->DeleteLocalRef(th);
    return out;
}

}  // namespace

struct Vm::Impl {
    LibHandle libjvm = nullptr;
    JavaVM*   vm     = nullptr;
    JNIEnv*   env    = nullptr;   // env for the thread that created the VM
    std::string lastError;
    bool started = false;
};

Vm::Vm() : impl_(new Impl) {}

Vm::~Vm() {
    if (impl_) {
        // We deliberately don't call DestroyJavaVM here: HotSpot does not allow
        // creating a second VM in the same process even after destroying the
        // first, and the spike's lifecycle is tied to process lifetime anyway.
        delete impl_;
        impl_ = nullptr;
    }
}

bool Vm::start(const InitOptions& opts) {
    if (impl_->started) {
        impl_->lastError = "Vm::start called twice";
        return false;
    }

    // Resolve JAVA_HOME.
    std::string javaHome = opts.javaHomeOverride;
    if (javaHome.empty()) {
        const char* env = std::getenv("JAVA_HOME");
        if (env && *env) javaHome = env;
    }
    if (javaHome.empty()) {
        impl_->lastError = "JAVA_HOME not set and no override provided";
        return false;
    }

    // Strip trailing slash for clean concatenation.
    while (!javaHome.empty() && (javaHome.back() == '/' || javaHome.back() == '\\')) {
        javaHome.pop_back();
    }

    const std::string libPath = javaHome + kJvmRelative;
    impl_->libjvm = loadLib(libPath.c_str());
    if (!impl_->libjvm) {
        impl_->lastError = "failed to load jvm library at " + libPath + ": " + lastSysError();
        return false;
    }

    auto createFn = reinterpret_cast<CreateJavaVMFn>(loadSym(impl_->libjvm, "JNI_CreateJavaVM"));
    if (!createFn) {
        impl_->lastError = "failed to resolve JNI_CreateJavaVM in " + libPath;
        return false;
    }

    // Build classpath option. Lifetime: vector of std::string holds the bytes
    // for the duration of the JNI_CreateJavaVM call; that's all that's needed.
    std::vector<std::string> optionStrings;
    if (!opts.classpath.empty()) {
        optionStrings.emplace_back("-Djava.class.path=" + joinClasspath(opts.classpath));
    }
    for (const auto& o : opts.jvmOptions) optionStrings.push_back(o);

    std::vector<JavaVMOption> jvmOpts;
    jvmOpts.reserve(optionStrings.size());
    for (auto& s : optionStrings) {
        JavaVMOption o{};
        o.optionString = const_cast<char*>(s.c_str());
        jvmOpts.push_back(o);
    }

    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8;
    args.nOptions = static_cast<jint>(jvmOpts.size());
    args.options = jvmOpts.empty() ? nullptr : jvmOpts.data();
    args.ignoreUnrecognized = JNI_FALSE;

    JavaVM* vm = nullptr;
    void* envRaw = nullptr;
    jint rc = createFn(&vm, &envRaw, &args);
    if (rc != JNI_OK || !vm || !envRaw) {
        std::ostringstream os;
        os << "JNI_CreateJavaVM returned " << rc;
        impl_->lastError = os.str();
        return false;
    }

    impl_->vm = vm;
    impl_->env = static_cast<JNIEnv*>(envRaw);
    impl_->started = true;
    impl_->lastError.clear();
    return true;
}

bool Vm::isStarted() const noexcept {
    return impl_ && impl_->started;
}

const std::string& Vm::lastError() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->lastError : kEmpty;
}

JavaVM* Vm::javaVm() const noexcept {
    return impl_ ? impl_->vm : nullptr;
}

// ------------------------------------------------------------------------
// ThreadGuard — RAII attach/detach for non-JVM-created caller threads.
// ------------------------------------------------------------------------
ThreadGuard::ThreadGuard(Vm& vm)
: vm_(vm.javaVm()), env_(nullptr), weAttached_(false) {
    if (!vm_) return;

    // Already attached? GetEnv returns JNI_OK and a valid env.
    void* envRaw = nullptr;
    jint rc = vm_->GetEnv(&envRaw, JNI_VERSION_1_8);
    if (rc == JNI_OK && envRaw) {
        env_ = static_cast<JNIEnv*>(envRaw);
        return;
    }
    // Detached. Attach as daemon so JVM shutdown is not blocked by us.
    JavaVMAttachArgs args{};
    args.version = JNI_VERSION_1_8;
    args.name = const_cast<char*>("jvm_embed_worker");
    args.group = nullptr;
    rc = vm_->AttachCurrentThreadAsDaemon(&envRaw, &args);
    if (rc == JNI_OK && envRaw) {
        env_ = static_cast<JNIEnv*>(envRaw);
        weAttached_ = true;
    }
}

ThreadGuard::~ThreadGuard() {
    if (weAttached_ && vm_) {
        vm_->DetachCurrentThread();
    }
}

namespace {
// RAII helper for JNI local-reference frames. With the Invocation API,
// local refs allocated by JNI calls are NOT freed at function return — they
// accumulate until the thread detaches. For a high-throughput wrapper called
// thousands of times per second during slow-start replay, that's a memory
// leak that scales with chain length. PushLocalFrame allocates a fresh frame
// with a known capacity; PopLocalFrame drops every ref allocated since.
struct LocalFrame {
    JNIEnv* env;
    bool    active;
    LocalFrame(JNIEnv* e, jint capacity) : env(e), active(false) {
        if (env && env->PushLocalFrame(capacity) == JNI_OK) active = true;
    }
    ~LocalFrame() {
        if (active) env->PopLocalFrame(nullptr);
    }
    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;
};
}  // namespace

namespace {

// Find a class by JVM name (e.g. "HelloEmbed", or "java/lang/String").
jclass findClassChecked(JNIEnv* env, const std::string& className, std::string& errOut) {
    jclass cls = env->FindClass(className.c_str());
    if (!cls || env->ExceptionCheck()) {
        errOut = "FindClass(" + className + ") failed: " + drainPendingException(env);
        return nullptr;
    }
    return cls;
}

jmethodID findStaticMethodChecked(JNIEnv* env, jclass cls, const std::string& name,
                                  const std::string& signature, std::string& errOut) {
    jmethodID mid = env->GetStaticMethodID(cls, name.c_str(), signature.c_str());
    if (!mid || env->ExceptionCheck()) {
        errOut = "GetStaticMethodID(" + name + signature + ") failed: " + drainPendingException(env);
        return nullptr;
    }
    return mid;
}

// ----- Type marshalling helpers ----------------------------------------------
// Centralised so future wrappers don't duplicate ref-management. Each helper
// returns a local ref (when constructing) which the caller must DeleteLocalRef,
// or returns a value (when extracting) and releases internal refs itself.

// std::string -> jstring (local ref). Returns nullptr on JNI error; check
// errOut for the description.
jstring toJString(JNIEnv* env, const std::string& s, std::string& errOut) {
    jstring js = env->NewStringUTF(s.c_str());
    if (!js || env->ExceptionCheck()) {
        errOut = "NewStringUTF failed: " + drainPendingException(env);
        return nullptr;
    }
    return js;
}

// jstring -> std::string. Caller still owns the jstring local ref.
std::optional<std::string> fromJString(JNIEnv* env, jstring js) {
    if (!js) return std::nullopt;
    const char* utf = env->GetStringUTFChars(js, nullptr);
    if (!utf) return std::nullopt;
    std::string out(utf);
    env->ReleaseStringUTFChars(js, utf);
    return out;
}

// std::vector<uint8_t> -> jbyteArray (local ref).
jbyteArray toJByteArray(JNIEnv* env, const std::vector<uint8_t>& bytes, std::string& errOut) {
    const jsize n = static_cast<jsize>(bytes.size());
    jbyteArray arr = env->NewByteArray(n);
    if (!arr || env->ExceptionCheck()) {
        errOut = "NewByteArray failed: " + drainPendingException(env);
        return nullptr;
    }
    if (n > 0) {
        env->SetByteArrayRegion(arr, 0, n, reinterpret_cast<const jbyte*>(bytes.data()));
        if (env->ExceptionCheck()) {
            errOut = "SetByteArrayRegion failed: " + drainPendingException(env);
            env->DeleteLocalRef(arr);
            return nullptr;
        }
    }
    return arr;
}

// jbyteArray -> std::vector<uint8_t>. Caller still owns the array local ref.
std::optional<std::vector<uint8_t>> fromJByteArray(JNIEnv* env, jbyteArray arr) {
    if (!arr) return std::nullopt;
    const jsize n = env->GetArrayLength(arr);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    if (n > 0) {
        env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(out.data()));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return std::nullopt;
        }
    }
    return out;
}

}  // namespace

std::optional<std::string> Vm::callStaticStringNoArg(const std::string& className,
                                                     const std::string& methodName) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass cls = findClassChecked(env, className, impl_->lastError);
    if (!cls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(env, cls, methodName, "()Ljava/lang/String;", impl_->lastError);
    if (!mid) { env->DeleteLocalRef(cls); return std::nullopt; }

    jobject ret = env->CallStaticObjectMethod(cls, mid);
    if (env->ExceptionCheck()) {
        impl_->lastError = "CallStaticObjectMethod(" + className + "." + methodName + ") threw: "
                           + drainPendingException(env);
        env->DeleteLocalRef(cls);
        return std::nullopt;
    }

    std::optional<std::string> result;
    if (ret) {
        jstring jstr = static_cast<jstring>(ret);
        const char* utf = env->GetStringUTFChars(jstr, nullptr);
        if (utf) {
            result = std::string(utf);
            env->ReleaseStringUTFChars(jstr, utf);
        }
        env->DeleteLocalRef(ret);
    }
    env->DeleteLocalRef(cls);
    impl_->lastError.clear();
    return result;
}

std::optional<std::string> Vm::callStaticStringOneStringArg(const std::string& className,
                                                            const std::string& methodName,
                                                            const std::string& arg) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass cls = findClassChecked(env, className, impl_->lastError);
    if (!cls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(env, cls, methodName,
                                            "(Ljava/lang/String;)Ljava/lang/String;",
                                            impl_->lastError);
    if (!mid) { env->DeleteLocalRef(cls); return std::nullopt; }

    jstring jarg = env->NewStringUTF(arg.c_str());
    if (!jarg || env->ExceptionCheck()) {
        impl_->lastError = "NewStringUTF failed: " + drainPendingException(env);
        env->DeleteLocalRef(cls);
        return std::nullopt;
    }

    jobject ret = env->CallStaticObjectMethod(cls, mid, jarg);
    env->DeleteLocalRef(jarg);

    if (env->ExceptionCheck()) {
        impl_->lastError = "CallStaticObjectMethod(" + className + "." + methodName + ") threw: "
                           + drainPendingException(env);
        env->DeleteLocalRef(cls);
        return std::nullopt;
    }

    std::optional<std::string> result;
    if (ret) {
        jstring jstr = static_cast<jstring>(ret);
        const char* utf = env->GetStringUTFChars(jstr, nullptr);
        if (utf) {
            result = std::string(utf);
            env->ReleaseStringUTFChars(jstr, utf);
        }
        env->DeleteLocalRef(ret);
    }
    env->DeleteLocalRef(cls);
    impl_->lastError.clear();
    return result;
}

std::optional<std::vector<uint8_t>>
Vm::callStaticBytesOneBytesArg(const std::string& className,
                               const std::string& methodName,
                               const std::vector<uint8_t>& arg) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass cls = findClassChecked(env, className, impl_->lastError);
    if (!cls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(env, cls, methodName, "([B)[B", impl_->lastError);
    if (!mid) { env->DeleteLocalRef(cls); return std::nullopt; }

    jbyteArray jarg = toJByteArray(env, arg, impl_->lastError);
    if (!jarg) { env->DeleteLocalRef(cls); return std::nullopt; }

    jobject ret = env->CallStaticObjectMethod(cls, mid, jarg);
    env->DeleteLocalRef(jarg);
    if (env->ExceptionCheck()) {
        impl_->lastError = "CallStaticObjectMethod(" + className + "." + methodName + ") threw: "
                           + drainPendingException(env);
        env->DeleteLocalRef(cls);
        return std::nullopt;
    }

    auto out = fromJByteArray(env, static_cast<jbyteArray>(ret));
    if (ret) env->DeleteLocalRef(ret);
    env->DeleteLocalRef(cls);
    impl_->lastError.clear();
    return out;
}

std::optional<int32_t> Vm::callStaticIntTwoInts(const std::string& className,
                                                const std::string& methodName,
                                                int32_t a, int32_t b) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass cls = findClassChecked(env, className, impl_->lastError);
    if (!cls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(env, cls, methodName, "(II)I", impl_->lastError);
    if (!mid) { env->DeleteLocalRef(cls); return std::nullopt; }

    jint ret = env->CallStaticIntMethod(cls, mid, static_cast<jint>(a), static_cast<jint>(b));
    if (env->ExceptionCheck()) {
        impl_->lastError = "CallStaticIntMethod(" + className + "." + methodName + ") threw: "
                           + drainPendingException(env);
        env->DeleteLocalRef(cls);
        return std::nullopt;
    }
    env->DeleteLocalRef(cls);
    impl_->lastError.clear();
    return static_cast<int32_t>(ret);
}

// ----- Cached-handle path ----------------------------------------------------

StaticMethodHandle Vm::resolveStaticMethod(const std::string& className,
                                           const std::string& methodName,
                                           const std::string& signature) {
    StaticMethodHandle h;
    if (!impl_->started) { impl_->lastError = "VM not started"; return h; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return h; }
    JNIEnv* env = guard.env();

    jclass localCls = findClassChecked(env, className, impl_->lastError);
    if (!localCls) return h;
    jmethodID mid = findStaticMethodChecked(env, localCls, methodName, signature, impl_->lastError);
    if (!mid) { env->DeleteLocalRef(localCls); return h; }

    // Promote to global so the class reference outlives this call frame.
    jobject globalCls = env->NewGlobalRef(localCls);
    env->DeleteLocalRef(localCls);
    if (!globalCls) {
        impl_->lastError = "NewGlobalRef failed for " + className;
        return h;
    }

    h.cls = static_cast<void*>(globalCls);
    h.mid = static_cast<void*>(mid);
    impl_->lastError.clear();
    return h;
}

void Vm::release(StaticMethodHandle& h) {
    if (!h.cls) { h = {}; return; }
    if (impl_->started) {
        ThreadGuard guard(*this);
        if (guard.ok()) {
            guard.env()->DeleteGlobalRef(static_cast<jobject>(h.cls));
        }
    }
    h = {};
}

std::optional<int32_t> Vm::callStaticIntTwoIntsCached(const StaticMethodHandle& h,
                                                      int32_t a, int32_t b) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (!h.valid())      { impl_->lastError = "invalid handle";  return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    auto cls = static_cast<jclass>(h.cls);
    auto mid = static_cast<jmethodID>(h.mid);
    jint ret = env->CallStaticIntMethod(cls, mid, static_cast<jint>(a), static_cast<jint>(b));
    if (env->ExceptionCheck()) {
        impl_->lastError = "cached CallStaticIntMethod threw: " + drainPendingException(env);
        return std::nullopt;
    }
    impl_->lastError.clear();
    return static_cast<int32_t>(ret);
}

// ----- Stub of getExecutorBuildVersion ---------------------------------------
// First end-to-end JNI wrapper of a service-shaped method. The Java side
// returns a StubExecutor.BuildVersionResult; we use Get*Field calls to
// extract each field. This pattern scales to wrapping the real Thrift
// result objects (ExecuteByteCodeResult etc.).

std::optional<Vm::ExecutorBuildVersionResult>
Vm::callGetExecutorBuildVersion(int16_t version, const std::string& className) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 64);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass execCls = findClassChecked(env, className, impl_->lastError);
    if (!execCls) return std::nullopt;
    const std::string sig = "(S)L" + className + "$BuildVersionResult;";
    jmethodID mid = findStaticMethodChecked(env, execCls, "getExecutorBuildVersion",
                                            sig, impl_->lastError);
    if (!mid) { env->DeleteLocalRef(execCls); return std::nullopt; }

    jobject result = env->CallStaticObjectMethod(execCls, mid, static_cast<jshort>(version));
    if (env->ExceptionCheck()) {
        impl_->lastError = "getExecutorBuildVersion threw: " + drainPendingException(env);
        env->DeleteLocalRef(execCls);
        return std::nullopt;
    }
    env->DeleteLocalRef(execCls);
    if (!result) {
        impl_->lastError = "getExecutorBuildVersion returned null";
        return std::nullopt;
    }

    // Extract fields from BuildVersionResult.
    jclass resCls = env->GetObjectClass(result);
    if (!resCls) {
        impl_->lastError = "GetObjectClass failed for BuildVersionResult";
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    jfieldID fCode    = env->GetFieldID(resCls, "code",         "B");
    jfieldID fMessage = env->GetFieldID(resCls, "message",      "Ljava/lang/String;");
    jfieldID fCommitN = env->GetFieldID(resCls, "commitNumber", "I");
    jfieldID fCommitH = env->GetFieldID(resCls, "commitHash",   "Ljava/lang/String;");
    if (!fCode || !fMessage || !fCommitN || !fCommitH || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on BuildVersionResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    ExecutorBuildVersionResult out;
    out.code         = static_cast<int8_t>(env->GetByteField(result, fCode));
    out.commitNumber = static_cast<int32_t>(env->GetIntField(result, fCommitN));

    jobject msgObj = env->GetObjectField(result, fMessage);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }
    jobject hashObj = env->GetObjectField(result, fCommitH);
    if (hashObj) {
        if (auto s = fromJString(env, static_cast<jstring>(hashObj))) out.commitHash = *s;
        env->DeleteLocalRef(hashObj);
    }

    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);
    impl_->lastError.clear();
    return out;
}

// ----- compileSourceCode -----------------------------------------------------
// Hits EmbeddedExecutorBridge.compileSourceCode in Java, which itself reaches
// into the production Dagger graph for a fully-wired ContractExecutorHandler.
// Output is a CompileResult with an array of CompiledClass (name + byte[]).
//
// This is the first wrapper involving an array of structs — the pattern of
// extracting parallel arrays via Java reflection-free field access is what
// the remaining 4 ContractExecutor methods will follow.

std::optional<Vm::CompileSourceCodeResult>
Vm::callCompileSourceCode(const std::string& source, int16_t version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 256);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "compileSourceCode",
        "(Ljava/lang/String;S)LEmbeddedExecutorBridge$CompileResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    jstring jsrc = toJString(env, source, impl_->lastError);
    if (!jsrc) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    jobject result = env->CallStaticObjectMethod(bridgeCls, mid, jsrc, static_cast<jshort>(version));
    env->DeleteLocalRef(jsrc);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "compileSourceCode threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "compileSourceCode returned null";
        return std::nullopt;
    }

    jclass resCls = env->GetObjectClass(result);
    if (!resCls) {
        impl_->lastError = "GetObjectClass failed for CompileResult";
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    jfieldID fCode    = env->GetFieldID(resCls, "code",    "B");
    jfieldID fMessage = env->GetFieldID(resCls, "message", "Ljava/lang/String;");
    jfieldID fClasses = env->GetFieldID(resCls, "classes", "[LEmbeddedExecutorBridge$CompiledClass;");
    if (!fCode || !fMessage || !fClasses || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on CompileResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    CompileSourceCodeResult out;
    out.code = static_cast<int8_t>(env->GetByteField(result, fCode));

    jobject msgObj = env->GetObjectField(result, fMessage);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }

    jobjectArray arr = static_cast<jobjectArray>(env->GetObjectField(result, fClasses));
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);

    if (arr) {
        const jsize n = env->GetArrayLength(arr);
        out.classes.reserve(static_cast<size_t>(n));

        // Resolve CompiledClass field IDs once — they're stable across all
        // elements of the array.
        jclass elemCls = nullptr;
        jfieldID fName = nullptr, fByteCode = nullptr;
        if (n > 0) {
            jobject probe = env->GetObjectArrayElement(arr, 0);
            if (probe) {
                elemCls = env->GetObjectClass(probe);
                fName     = env->GetFieldID(elemCls, "name",     "Ljava/lang/String;");
                fByteCode = env->GetFieldID(elemCls, "byteCode", "[B");
                env->DeleteLocalRef(probe);
            }
            if (!elemCls || !fName || !fByteCode) {
                impl_->lastError = "GetFieldID failed on CompiledClass: " + drainPendingException(env);
                if (elemCls) env->DeleteLocalRef(elemCls);
                env->DeleteLocalRef(arr);
                return std::nullopt;
            }
        }

        for (jsize i = 0; i < n; ++i) {
            jobject elem = env->GetObjectArrayElement(arr, i);
            if (!elem) continue;

            CompiledClass cc;
            jobject nameObj = env->GetObjectField(elem, fName);
            if (nameObj) {
                if (auto s = fromJString(env, static_cast<jstring>(nameObj))) cc.name = *s;
                env->DeleteLocalRef(nameObj);
            }
            jbyteArray bcArr = static_cast<jbyteArray>(env->GetObjectField(elem, fByteCode));
            if (bcArr) {
                if (auto b = fromJByteArray(env, bcArr)) cc.byteCode = std::move(*b);
                env->DeleteLocalRef(bcArr);
            }
            out.classes.push_back(std::move(cc));
            env->DeleteLocalRef(elem);
        }
        if (elemCls) env->DeleteLocalRef(elemCls);
        env->DeleteLocalRef(arr);
    }

    impl_->lastError.clear();
    return out;
}

// ----- getContractMethods ---------------------------------------------------
// Two-way List<Struct> marshalling: C++ -> Java for the input bytecode list,
// Java -> C++ for the introspected MethodDescription list. Together with
// compileSourceCode this proves the full input-and-output struct pattern that
// the remaining Thrift methods will reuse.

std::optional<Vm::GetContractMethodsResult>
Vm::callGetContractMethods(const std::vector<std::string>&          classNames,
                           const std::vector<std::vector<uint8_t>>& byteCodes,
                           int16_t                                  version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (classNames.size() != byteCodes.size()) {
        impl_->lastError = "classNames / byteCodes length mismatch";
        return std::nullopt;
    }

    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 256);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "getContractMethods",
        "([Ljava/lang/String;[[BS)LEmbeddedExecutorBridge$MethodsResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    // Build String[] of class names.
    jclass stringCls = env->FindClass("java/lang/String");
    if (!stringCls) {
        impl_->lastError = "FindClass(java/lang/String) failed";
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }
    jobjectArray jnames = env->NewObjectArray(static_cast<jsize>(classNames.size()), stringCls, nullptr);
    if (!jnames) {
        impl_->lastError = "NewObjectArray(String[]) failed: " + drainPendingException(env);
        env->DeleteLocalRef(stringCls);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }
    for (size_t i = 0; i < classNames.size(); ++i) {
        jstring js = toJString(env, classNames[i], impl_->lastError);
        if (!js) {
            env->DeleteLocalRef(jnames);
            env->DeleteLocalRef(stringCls);
            env->DeleteLocalRef(bridgeCls);
            return std::nullopt;
        }
        env->SetObjectArrayElement(jnames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(stringCls);

    // Build byte[][] from the vector of byte vectors.
    jclass byteArrCls = env->FindClass("[B");
    if (!byteArrCls) {
        impl_->lastError = "FindClass([B) failed";
        env->DeleteLocalRef(jnames);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }
    jobjectArray jbcs = env->NewObjectArray(static_cast<jsize>(byteCodes.size()), byteArrCls, nullptr);
    if (!jbcs) {
        impl_->lastError = "NewObjectArray(byte[][]) failed: " + drainPendingException(env);
        env->DeleteLocalRef(byteArrCls);
        env->DeleteLocalRef(jnames);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }
    for (size_t i = 0; i < byteCodes.size(); ++i) {
        jbyteArray ja = toJByteArray(env, byteCodes[i], impl_->lastError);
        if (!ja) {
            env->DeleteLocalRef(jbcs);
            env->DeleteLocalRef(byteArrCls);
            env->DeleteLocalRef(jnames);
            env->DeleteLocalRef(bridgeCls);
            return std::nullopt;
        }
        env->SetObjectArrayElement(jbcs, static_cast<jsize>(i), ja);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(byteArrCls);

    jobject result = env->CallStaticObjectMethod(bridgeCls, mid, jnames, jbcs, static_cast<jshort>(version));
    env->DeleteLocalRef(jnames);
    env->DeleteLocalRef(jbcs);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "getContractMethods threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "getContractMethods returned null";
        return std::nullopt;
    }

    jclass resCls = env->GetObjectClass(result);
    jfieldID fCode    = env->GetFieldID(resCls, "code",    "B");
    jfieldID fMessage = env->GetFieldID(resCls, "message", "Ljava/lang/String;");
    jfieldID fMethods = env->GetFieldID(resCls, "methods", "[LEmbeddedExecutorBridge$MethodInfo;");
    if (!fCode || !fMessage || !fMethods || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on MethodsResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    GetContractMethodsResult out;
    out.code = static_cast<int8_t>(env->GetByteField(result, fCode));
    jobject msgObj = env->GetObjectField(result, fMessage);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }
    jobjectArray methodsArr = static_cast<jobjectArray>(env->GetObjectField(result, fMethods));
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);

    if (methodsArr) {
        const jsize n = env->GetArrayLength(methodsArr);
        out.methods.reserve(static_cast<size_t>(n));

        // Lazy-resolve field IDs for MethodInfo, MethodArgumentInfo, AnnotationInfo.
        jclass miCls = nullptr;
        jfieldID fName = nullptr, fRtype = nullptr, fArgs = nullptr, fAnns = nullptr;
        jclass argCls = nullptr;
        jfieldID fArgType = nullptr, fArgName = nullptr, fArgAnns = nullptr;
        jclass annCls = nullptr;
        jfieldID fAnnName = nullptr, fAnnKeys = nullptr, fAnnVals = nullptr;

        auto extractAnnotations = [&](jobjectArray annsArr) -> std::vector<AnnotationInfo> {
            std::vector<AnnotationInfo> result;
            if (!annsArr) return result;
            const jsize an = env->GetArrayLength(annsArr);
            for (jsize k = 0; k < an; ++k) {
                jobject annElem = env->GetObjectArrayElement(annsArr, k);
                if (!annElem) continue;
                if (!annCls) {
                    annCls = env->GetObjectClass(annElem);
                    fAnnName = env->GetFieldID(annCls, "name",      "Ljava/lang/String;");
                    fAnnKeys = env->GetFieldID(annCls, "argNames",  "[Ljava/lang/String;");
                    fAnnVals = env->GetFieldID(annCls, "argValues", "[Ljava/lang/String;");
                }
                AnnotationInfo ai;
                jobject nameObj = env->GetObjectField(annElem, fAnnName);
                if (nameObj) {
                    if (auto s = fromJString(env, static_cast<jstring>(nameObj))) ai.name = *s;
                    env->DeleteLocalRef(nameObj);
                }
                jobjectArray keys = static_cast<jobjectArray>(env->GetObjectField(annElem, fAnnKeys));
                jobjectArray vals = static_cast<jobjectArray>(env->GetObjectField(annElem, fAnnVals));
                if (keys && vals) {
                    const jsize kn = std::min(env->GetArrayLength(keys), env->GetArrayLength(vals));
                    for (jsize x = 0; x < kn; ++x) {
                        std::string k1, v1;
                        jobject ko = env->GetObjectArrayElement(keys, x);
                        if (ko) { if (auto s = fromJString(env, static_cast<jstring>(ko))) k1 = *s; env->DeleteLocalRef(ko); }
                        jobject vo = env->GetObjectArrayElement(vals, x);
                        if (vo) { if (auto s = fromJString(env, static_cast<jstring>(vo))) v1 = *s; env->DeleteLocalRef(vo); }
                        ai.arguments.emplace_back(std::move(k1), std::move(v1));
                    }
                }
                if (keys) env->DeleteLocalRef(keys);
                if (vals) env->DeleteLocalRef(vals);
                env->DeleteLocalRef(annElem);
                result.push_back(std::move(ai));
            }
            return result;
        };

        for (jsize i = 0; i < n; ++i) {
            jobject elem = env->GetObjectArrayElement(methodsArr, i);
            if (!elem) continue;
            if (!miCls) {
                miCls  = env->GetObjectClass(elem);
                fName  = env->GetFieldID(miCls, "name",        "Ljava/lang/String;");
                fRtype = env->GetFieldID(miCls, "returnType",  "Ljava/lang/String;");
                fArgs  = env->GetFieldID(miCls, "arguments",   "[LEmbeddedExecutorBridge$MethodArgumentInfo;");
                fAnns  = env->GetFieldID(miCls, "annotations", "[LEmbeddedExecutorBridge$AnnotationInfo;");
            }

            MethodInfo mi;
            jobject nameObj = env->GetObjectField(elem, fName);
            if (nameObj) {
                if (auto s = fromJString(env, static_cast<jstring>(nameObj))) mi.name = *s;
                env->DeleteLocalRef(nameObj);
            }
            jobject rtypeObj = env->GetObjectField(elem, fRtype);
            if (rtypeObj) {
                if (auto s = fromJString(env, static_cast<jstring>(rtypeObj))) mi.returnType = *s;
                env->DeleteLocalRef(rtypeObj);
            }

            // arguments
            jobjectArray argsArr = static_cast<jobjectArray>(env->GetObjectField(elem, fArgs));
            if (argsArr) {
                const jsize an = env->GetArrayLength(argsArr);
                for (jsize j = 0; j < an; ++j) {
                    jobject argElem = env->GetObjectArrayElement(argsArr, j);
                    if (!argElem) continue;
                    if (!argCls) {
                        argCls   = env->GetObjectClass(argElem);
                        fArgType = env->GetFieldID(argCls, "type",        "Ljava/lang/String;");
                        fArgName = env->GetFieldID(argCls, "name",        "Ljava/lang/String;");
                        fArgAnns = env->GetFieldID(argCls, "annotations", "[LEmbeddedExecutorBridge$AnnotationInfo;");
                    }
                    MethodArgumentInfo mai;
                    jobject typeObj = env->GetObjectField(argElem, fArgType);
                    if (typeObj) { if (auto s = fromJString(env, static_cast<jstring>(typeObj))) mai.type = *s; env->DeleteLocalRef(typeObj); }
                    jobject nObj   = env->GetObjectField(argElem, fArgName);
                    if (nObj)    { if (auto s = fromJString(env, static_cast<jstring>(nObj))) mai.name = *s; env->DeleteLocalRef(nObj); }
                    jobjectArray innerAnns = static_cast<jobjectArray>(env->GetObjectField(argElem, fArgAnns));
                    if (innerAnns) {
                        mai.annotations = extractAnnotations(innerAnns);
                        env->DeleteLocalRef(innerAnns);
                    }
                    mi.arguments.push_back(std::move(mai));
                    env->DeleteLocalRef(argElem);
                }
                env->DeleteLocalRef(argsArr);
            }

            // annotations on the method itself
            jobjectArray annsArr = static_cast<jobjectArray>(env->GetObjectField(elem, fAnns));
            if (annsArr) {
                mi.annotations = extractAnnotations(annsArr);
                env->DeleteLocalRef(annsArr);
            }

            out.methods.push_back(std::move(mi));
            env->DeleteLocalRef(elem);
        }
        if (miCls)  env->DeleteLocalRef(miCls);
        if (argCls) env->DeleteLocalRef(argCls);
        if (annCls) env->DeleteLocalRef(annCls);
        env->DeleteLocalRef(methodsArr);
    }

    impl_->lastError.clear();
    return out;
}

// ----- TaggedVariant extraction helper ---------------------------------------
// Mirror of EmbeddedExecutorBridge.TaggedVariant. Field IDs cached on first
// call (per-thread cache would be safer in a long-lived process, but this
// helper resolves them every call and is fast enough for the spike).
namespace {

struct TaggedVariantFieldIds {
    jclass    cls           = nullptr;
    jfieldID  fTag           = nullptr;
    jfieldID  fBool          = nullptr;
    jfieldID  fLong          = nullptr;
    jfieldID  fDouble        = nullptr;
    jfieldID  fString        = nullptr;
    jfieldID  fBytes         = nullptr;
    jfieldID  fRepr          = nullptr;
    bool      ok             = false;
};

TaggedVariantFieldIds resolveTaggedVariantFields(JNIEnv* env, jobject sample, std::string& errOut) {
    TaggedVariantFieldIds f;
    if (!sample) return f;
    f.cls    = env->GetObjectClass(sample);
    if (!f.cls) { errOut = "GetObjectClass failed for TaggedVariant"; return f; }
    f.fTag    = env->GetFieldID(f.cls, "tag",       "I");
    f.fBool   = env->GetFieldID(f.cls, "boolVal",   "Z");
    f.fLong   = env->GetFieldID(f.cls, "longVal",   "J");
    f.fDouble = env->GetFieldID(f.cls, "doubleVal", "D");
    f.fString = env->GetFieldID(f.cls, "stringVal", "Ljava/lang/String;");
    f.fBytes  = env->GetFieldID(f.cls, "bytesVal",  "[B");
    f.fRepr   = env->GetFieldID(f.cls, "repr",      "Ljava/lang/String;");
    if (!f.fTag || !f.fBool || !f.fLong || !f.fDouble || !f.fString || !f.fBytes || !f.fRepr || env->ExceptionCheck()) {
        errOut = "GetFieldID failed on TaggedVariant: " + drainPendingException(env);
        return f;
    }
    f.ok = true;
    return f;
}

Vm::Variant readTaggedVariant(JNIEnv* env, jobject obj, const TaggedVariantFieldIds& f) {
    Vm::Variant v;
    if (!obj || !f.ok) return v;
    int tag = static_cast<int>(env->GetIntField(obj, f.fTag));
    if (tag < 0 || tag > Vm::VTAG_THRIFT_BINARY) tag = Vm::VTAG_OTHER;
    v.tag = static_cast<Vm::VariantTag>(tag);
    v.boolVal   = env->GetBooleanField(obj, f.fBool) != JNI_FALSE;
    v.longVal   = static_cast<int64_t>(env->GetLongField(obj, f.fLong));
    v.doubleVal = env->GetDoubleField(obj, f.fDouble);
    jobject sObj = env->GetObjectField(obj, f.fString);
    if (sObj) {
        if (auto s = fromJString(env, static_cast<jstring>(sObj))) v.stringVal = *s;
        env->DeleteLocalRef(sObj);
    }
    jobject bArr = env->GetObjectField(obj, f.fBytes);
    if (bArr) {
        if (auto b = fromJByteArray(env, static_cast<jbyteArray>(bArr))) v.bytesVal = std::move(*b);
        env->DeleteLocalRef(bArr);
    }
    jobject rObj = env->GetObjectField(obj, f.fRepr);
    if (rObj) {
        if (auto s = fromJString(env, static_cast<jstring>(rObj))) v.repr = *s;
        env->DeleteLocalRef(rObj);
    }
    return v;
}

}  // namespace

// ----- getContractVariables --------------------------------------------------

std::optional<Vm::GetContractVariablesResult>
Vm::callGetContractVariables(const std::vector<std::string>&          classNames,
                             const std::vector<std::vector<uint8_t>>& byteCodes,
                             const std::vector<uint8_t>&              state,
                             int16_t                                  version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (classNames.size() != byteCodes.size()) {
        impl_->lastError = "classNames / byteCodes length mismatch"; return std::nullopt;
    }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 256);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "getContractVariables",
        "([Ljava/lang/String;[[B[BS)LEmbeddedExecutorBridge$VariablesResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    // Build String[] of class names.
    jclass stringCls = env->FindClass("java/lang/String");
    jobjectArray jnames = env->NewObjectArray(static_cast<jsize>(classNames.size()), stringCls, nullptr);
    for (size_t i = 0; i < classNames.size(); ++i) {
        jstring js = toJString(env, classNames[i], impl_->lastError);
        if (!js) {
            env->DeleteLocalRef(jnames);
            env->DeleteLocalRef(stringCls);
            env->DeleteLocalRef(bridgeCls);
            return std::nullopt;
        }
        env->SetObjectArrayElement(jnames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(stringCls);

    // Build byte[][] of bytecodes.
    jclass byteArrCls = env->FindClass("[B");
    jobjectArray jbcs = env->NewObjectArray(static_cast<jsize>(byteCodes.size()), byteArrCls, nullptr);
    for (size_t i = 0; i < byteCodes.size(); ++i) {
        jbyteArray ja = toJByteArray(env, byteCodes[i], impl_->lastError);
        if (!ja) {
            env->DeleteLocalRef(jbcs);
            env->DeleteLocalRef(byteArrCls);
            env->DeleteLocalRef(jnames);
            env->DeleteLocalRef(bridgeCls);
            return std::nullopt;
        }
        env->SetObjectArrayElement(jbcs, static_cast<jsize>(i), ja);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(byteArrCls);

    // State as byte[].
    jbyteArray jstate = toJByteArray(env, state, impl_->lastError);
    if (!jstate) {
        env->DeleteLocalRef(jnames);
        env->DeleteLocalRef(jbcs);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }

    jobject result = env->CallStaticObjectMethod(bridgeCls, mid, jnames, jbcs, jstate, static_cast<jshort>(version));
    env->DeleteLocalRef(jnames);
    env->DeleteLocalRef(jbcs);
    env->DeleteLocalRef(jstate);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "getContractVariables threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "getContractVariables returned null";
        return std::nullopt;
    }

    jclass resCls    = env->GetObjectClass(result);
    jfieldID fCode   = env->GetFieldID(resCls, "code",    "B");
    jfieldID fMsg    = env->GetFieldID(resCls, "message", "Ljava/lang/String;");
    jfieldID fNames  = env->GetFieldID(resCls, "names",   "[Ljava/lang/String;");
    jfieldID fValues = env->GetFieldID(resCls, "values",  "[LEmbeddedExecutorBridge$TaggedVariant;");
    if (!fCode || !fMsg || !fNames || !fValues || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on VariablesResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    GetContractVariablesResult out;
    out.code = static_cast<int8_t>(env->GetByteField(result, fCode));
    jobject msgObj = env->GetObjectField(result, fMsg);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }

    jobjectArray namesArr  = static_cast<jobjectArray>(env->GetObjectField(result, fNames));
    jobjectArray valuesArr = static_cast<jobjectArray>(env->GetObjectField(result, fValues));
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);

    if (namesArr && valuesArr) {
        const jsize nN = env->GetArrayLength(namesArr);
        const jsize nV = env->GetArrayLength(valuesArr);
        const jsize n  = std::min(nN, nV);

        TaggedVariantFieldIds fIds;
        if (n > 0) {
            jobject probe = env->GetObjectArrayElement(valuesArr, 0);
            fIds = resolveTaggedVariantFields(env, probe, impl_->lastError);
            if (probe) env->DeleteLocalRef(probe);
            if (!fIds.ok) {
                env->DeleteLocalRef(namesArr);
                env->DeleteLocalRef(valuesArr);
                return std::nullopt;
            }
        }

        out.variables.reserve(static_cast<size_t>(n));
        for (jsize i = 0; i < n; ++i) {
            std::string name;
            jobject nameObj = env->GetObjectArrayElement(namesArr, i);
            if (nameObj) {
                if (auto s = fromJString(env, static_cast<jstring>(nameObj))) name = *s;
                env->DeleteLocalRef(nameObj);
            }
            jobject valObj = env->GetObjectArrayElement(valuesArr, i);
            Variant val = readTaggedVariant(env, valObj, fIds);
            if (valObj) env->DeleteLocalRef(valObj);
            out.variables.emplace_back(std::move(name), std::move(val));
        }
        if (fIds.cls) env->DeleteLocalRef(fIds.cls);
        env->DeleteLocalRef(namesArr);
        env->DeleteLocalRef(valuesArr);
    } else {
        if (namesArr)  env->DeleteLocalRef(namesArr);
        if (valuesArr) env->DeleteLocalRef(valuesArr);
    }

    impl_->lastError.clear();
    return out;
}

// ----- makeVariantSamples ----------------------------------------------------
// Synthetic round-trip; useful for testing the Variant pipeline without
// needing a deployed contract state.

std::optional<std::vector<Vm::Variant>> Vm::callMakeVariantSamples() {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "makeVariantSamples",
        "()[LEmbeddedExecutorBridge$TaggedVariant;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    jobject result = env->CallStaticObjectMethod(bridgeCls, mid);
    env->DeleteLocalRef(bridgeCls);
    if (env->ExceptionCheck()) {
        impl_->lastError = "makeVariantSamples threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "makeVariantSamples returned null";
        return std::nullopt;
    }

    jobjectArray arr = static_cast<jobjectArray>(result);
    const jsize n = env->GetArrayLength(arr);
    std::vector<Variant> out;

    TaggedVariantFieldIds fIds;
    if (n > 0) {
        jobject probe = env->GetObjectArrayElement(arr, 0);
        fIds = resolveTaggedVariantFields(env, probe, impl_->lastError);
        if (probe) env->DeleteLocalRef(probe);
        if (!fIds.ok) {
            env->DeleteLocalRef(arr);
            return std::nullopt;
        }
    }

    out.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) {
        jobject elem = env->GetObjectArrayElement(arr, i);
        out.push_back(readTaggedVariant(env, elem, fIds));
        if (elem) env->DeleteLocalRef(elem);
    }
    if (fIds.cls) env->DeleteLocalRef(fIds.cls);
    env->DeleteLocalRef(arr);
    impl_->lastError.clear();
    return out;
}

// ----- executeByteCode -------------------------------------------------------
// Largest wrapper in the spike: 9 inputs (mix of primitives, byte arrays,
// parallel string/byte-array lists) into a flat call to the bridge, which
// builds the nested Thrift struct on its side. Output is flattened back into
// a single ExecuteResult struct with the first method's return Variant.

std::optional<Vm::ExecuteResult>
Vm::callExecuteByteCode(int64_t                                  accessId,
                        const std::vector<uint8_t>&              initiatorAddress,
                        const std::vector<uint8_t>&              contractAddress,
                        const std::vector<std::string>&          classNames,
                        const std::vector<std::vector<uint8_t>>& byteCodes,
                        const std::vector<uint8_t>&              instance,
                        bool                                     stateCanModify,
                        const std::string&                       methodName,
                        int64_t                                  executionTimeoutMs,
                        int16_t                                  version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (classNames.size() != byteCodes.size()) {
        impl_->lastError = "classNames / byteCodes length mismatch"; return std::nullopt;
    }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "executeByteCode",
        "(J[B[B[Ljava/lang/String;[[B[BZLjava/lang/String;JS)LEmbeddedExecutorBridge$ExecuteResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    // Build Java arguments. Separate scope so error returns clean up.
    jbyteArray   jInitiator = toJByteArray(env, initiatorAddress, impl_->lastError);
    jbyteArray   jContract  = toJByteArray(env, contractAddress,  impl_->lastError);
    jbyteArray   jInstance  = toJByteArray(env, instance,         impl_->lastError);
    jstring      jMethod    = toJString  (env, methodName,        impl_->lastError);
    if (!jInitiator || !jContract || !jInstance || !jMethod) {
        if (jInitiator) env->DeleteLocalRef(jInitiator);
        if (jContract)  env->DeleteLocalRef(jContract);
        if (jInstance)  env->DeleteLocalRef(jInstance);
        if (jMethod)    env->DeleteLocalRef(jMethod);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }

    jclass stringCls = env->FindClass("java/lang/String");
    jobjectArray jnames = env->NewObjectArray(static_cast<jsize>(classNames.size()), stringCls, nullptr);
    for (size_t i = 0; i < classNames.size(); ++i) {
        jstring js = toJString(env, classNames[i], impl_->lastError);
        env->SetObjectArrayElement(jnames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(stringCls);

    jclass byteArrCls = env->FindClass("[B");
    jobjectArray jbcs = env->NewObjectArray(static_cast<jsize>(byteCodes.size()), byteArrCls, nullptr);
    for (size_t i = 0; i < byteCodes.size(); ++i) {
        jbyteArray ja = toJByteArray(env, byteCodes[i], impl_->lastError);
        env->SetObjectArrayElement(jbcs, static_cast<jsize>(i), ja);
        env->DeleteLocalRef(ja);
    }
    env->DeleteLocalRef(byteArrCls);

    jobject result = env->CallStaticObjectMethod(
        bridgeCls, mid,
        static_cast<jlong>(accessId),
        jInitiator, jContract,
        jnames, jbcs,
        jInstance, static_cast<jboolean>(stateCanModify ? JNI_TRUE : JNI_FALSE),
        jMethod,
        static_cast<jlong>(executionTimeoutMs),
        static_cast<jshort>(version));

    env->DeleteLocalRef(jInitiator);
    env->DeleteLocalRef(jContract);
    env->DeleteLocalRef(jnames);
    env->DeleteLocalRef(jbcs);
    env->DeleteLocalRef(jInstance);
    env->DeleteLocalRef(jMethod);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "executeByteCode threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "executeByteCode returned null";
        return std::nullopt;
    }

    jclass resCls       = env->GetObjectClass(result);
    jfieldID fCode       = env->GetFieldID(resCls, "code",          "B");
    jfieldID fMessage    = env->GetFieldID(resCls, "message",       "Ljava/lang/String;");
    jfieldID fMCode      = env->GetFieldID(resCls, "methodCode",    "B");
    jfieldID fMMessage   = env->GetFieldID(resCls, "methodMessage", "Ljava/lang/String;");
    jfieldID fRetVal     = env->GetFieldID(resCls, "retVal",        "LEmbeddedExecutorBridge$TaggedVariant;");
    jfieldID fCost       = env->GetFieldID(resCls, "executionCost", "J");
    jfieldID fNewState   = env->GetFieldID(resCls, "newState",      "[B");
    if (!fCode || !fMessage || !fMCode || !fMMessage || !fRetVal || !fCost || !fNewState
        || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on ExecuteResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    ExecuteResult out;
    out.code         = static_cast<int8_t>(env->GetByteField(result, fCode));
    out.methodCode   = static_cast<int8_t>(env->GetByteField(result, fMCode));
    out.executionCost = static_cast<int64_t>(env->GetLongField(result, fCost));

    jobject msgObj = env->GetObjectField(result, fMessage);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }
    jobject mMsgObj = env->GetObjectField(result, fMMessage);
    if (mMsgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(mMsgObj))) out.methodMessage = *s;
        env->DeleteLocalRef(mMsgObj);
    }
    jobject retObj = env->GetObjectField(result, fRetVal);
    if (retObj) {
        TaggedVariantFieldIds f = resolveTaggedVariantFields(env, retObj, impl_->lastError);
        if (f.ok) {
            out.retVal = readTaggedVariant(env, retObj, f);
            env->DeleteLocalRef(f.cls);
        }
        env->DeleteLocalRef(retObj);
    }
    jbyteArray nsArr = static_cast<jbyteArray>(env->GetObjectField(result, fNewState));
    if (nsArr) {
        if (auto b = fromJByteArray(env, nsArr)) out.newState = std::move(*b);
        env->DeleteLocalRef(nsArr);
    }

    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);
    impl_->lastError.clear();
    return out;
}

// ----- runVariantSelfTest ----------------------------------------------------
// Calls the bridge's runVariantSelfTest() and translates the CaseReport[]
// into a VariantSelfTestReport. Per-case pass/fail computed in C++ against
// the bridge-supplied expectedTag — so a tag-clamp bug on either side
// shows up as a failed case rather than going silent.

std::optional<Vm::VariantSelfTestReport> Vm::runVariantSelfTest() {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "runVariantSelfTest",
        "()[LEmbeddedExecutorBridge$CaseReport;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    jobject result = env->CallStaticObjectMethod(bridgeCls, mid);
    env->DeleteLocalRef(bridgeCls);
    if (env->ExceptionCheck()) {
        impl_->lastError = "runVariantSelfTest threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "runVariantSelfTest returned null";
        return std::nullopt;
    }

    jobjectArray arr = static_cast<jobjectArray>(result);
    const jsize n = env->GetArrayLength(arr);

    VariantSelfTestReport report;
    report.cases.reserve(static_cast<size_t>(n));

    jclass crCls = nullptr;
    jfieldID fName = nullptr, fExpected = nullptr, fFlattened = nullptr;
    TaggedVariantFieldIds tvFields;

    for (jsize i = 0; i < n; ++i) {
        jobject elem = env->GetObjectArrayElement(arr, i);
        if (!elem) continue;

        if (!crCls) {
            crCls       = env->GetObjectClass(elem);
            fName       = env->GetFieldID(crCls, "name",        "Ljava/lang/String;");
            fExpected   = env->GetFieldID(crCls, "expectedTag", "I");
            fFlattened  = env->GetFieldID(crCls, "flattened",   "LEmbeddedExecutorBridge$TaggedVariant;");
            if (!fName || !fExpected || !fFlattened) {
                impl_->lastError = "GetFieldID failed on CaseReport";
                env->DeleteLocalRef(elem);
                env->DeleteLocalRef(arr);
                return std::nullopt;
            }
        }

        VariantSelfTestCase tc;
        jobject nameObj = env->GetObjectField(elem, fName);
        if (nameObj) {
            if (auto s = fromJString(env, static_cast<jstring>(nameObj))) tc.name = *s;
            env->DeleteLocalRef(nameObj);
        }
        tc.expectedTag = static_cast<int32_t>(env->GetIntField(elem, fExpected));

        jobject tvObj = env->GetObjectField(elem, fFlattened);
        if (tvObj) {
            if (!tvFields.ok) tvFields = resolveTaggedVariantFields(env, tvObj, impl_->lastError);
            if (tvFields.ok) tc.actualTag = static_cast<int32_t>(env->GetIntField(tvObj, tvFields.fTag));
            env->DeleteLocalRef(tvObj);
        }

        tc.ok = (tc.actualTag == tc.expectedTag);
        if (tc.ok) ++report.passes;
        else       ++report.fails;
        report.cases.push_back(std::move(tc));

        env->DeleteLocalRef(elem);
    }
    if (crCls)         env->DeleteLocalRef(crCls);
    if (tvFields.cls)  env->DeleteLocalRef(tvFields.cls);
    env->DeleteLocalRef(arr);

    impl_->lastError.clear();
    return report;
}


// ----- callExecuteByteCodeFull ------------------------------------------------
// Production-fidelity executeByteCode wrapper. Returns the complete result
// shape so cs::Executor can populate Thrift's ExecuteByteCodeResult without
// dropping any field. Critical for chain replay correctness.

namespace {

struct FullStateEntryFieldIds {
    jclass cls = nullptr;
    jfieldID fAddr = nullptr, fState = nullptr;
    bool ok = false;
};
struct FullEmittedTxnFieldIds {
    jclass cls = nullptr;
    jfieldID fSrc = nullptr, fTgt = nullptr, fAi = nullptr, fAf = nullptr, fUd = nullptr;
    bool ok = false;
};
struct FullSetterResultFieldIds {
    jclass cls = nullptr;
    jfieldID fCode = nullptr, fMsg = nullptr, fRetVal = nullptr,
             fState = nullptr, fEmitted = nullptr, fCost = nullptr;
    bool ok = false;
};

FullStateEntryFieldIds resolveStateEntryFields(JNIEnv* env, jobject sample, std::string& errOut) {
    FullStateEntryFieldIds f;
    if (!sample) return f;
    f.cls    = env->GetObjectClass(sample);
    f.fAddr  = env->GetFieldID(f.cls, "address", "[B");
    f.fState = env->GetFieldID(f.cls, "state",   "[B");
    if (!f.fAddr || !f.fState || env->ExceptionCheck()) {
        errOut = "GetFieldID failed on FullStateEntry: " + drainPendingException(env);
        return f;
    }
    f.ok = true;
    return f;
}

FullEmittedTxnFieldIds resolveEmittedTxnFields(JNIEnv* env, jobject sample, std::string& errOut) {
    FullEmittedTxnFieldIds f;
    if (!sample) return f;
    f.cls = env->GetObjectClass(sample);
    f.fSrc = env->GetFieldID(f.cls, "source",         "[B");
    f.fTgt = env->GetFieldID(f.cls, "target",         "[B");
    f.fAi  = env->GetFieldID(f.cls, "amountIntegral", "I");
    f.fAf  = env->GetFieldID(f.cls, "amountFraction", "J");
    f.fUd  = env->GetFieldID(f.cls, "userData",       "[B");
    if (!f.fSrc || !f.fTgt || !f.fAi || !f.fAf || !f.fUd || env->ExceptionCheck()) {
        errOut = "GetFieldID failed on FullEmittedTxn: " + drainPendingException(env);
        return f;
    }
    f.ok = true;
    return f;
}

FullSetterResultFieldIds resolveSetterResultFields(JNIEnv* env, jobject sample, std::string& errOut) {
    FullSetterResultFieldIds f;
    if (!sample) return f;
    f.cls     = env->GetObjectClass(sample);
    f.fCode   = env->GetFieldID(f.cls, "code",                "B");
    f.fMsg    = env->GetFieldID(f.cls, "message",             "Ljava/lang/String;");
    f.fRetVal = env->GetFieldID(f.cls, "retVal",              "LEmbeddedExecutorBridge$TaggedVariant;");
    f.fState  = env->GetFieldID(f.cls, "contractsState",      "[LEmbeddedExecutorBridge$FullStateEntry;");
    f.fEmitted= env->GetFieldID(f.cls, "emittedTransactions", "[LEmbeddedExecutorBridge$FullEmittedTxn;");
    f.fCost   = env->GetFieldID(f.cls, "executionCost",       "J");
    if (!f.fCode || !f.fMsg || !f.fRetVal || !f.fState || !f.fEmitted || !f.fCost || env->ExceptionCheck()) {
        errOut = "GetFieldID failed on FullSetterResult: " + drainPendingException(env);
        return f;
    }
    f.ok = true;
    return f;
}

}  // namespace

std::optional<Vm::FullExecuteResult>
Vm::callExecuteByteCodeFull(int64_t                                  accessId,
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
                            int16_t                                  version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (classNames.size() != byteCodes.size()) {
        impl_->lastError = "classNames / byteCodes length mismatch"; return std::nullopt;
    }
    if (methodNames.size() != paramGroupSizes.size()) {
        impl_->lastError = "methodNames / paramGroupSizes length mismatch"; return std::nullopt;
    }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 256);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "executeByteCodeFull",
        "(J[B[B[Ljava/lang/String;[[B[BZ[Ljava/lang/String;[I[I[Z[J[D[Ljava/lang/String;[[BJS)"
        "LEmbeddedExecutorBridge$FullExecuteResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    // Build all the Java arguments.
    jbyteArray jInitiator = toJByteArray(env, initiatorAddress, impl_->lastError);
    jbyteArray jContract  = toJByteArray(env, contractAddress,  impl_->lastError);
    jbyteArray jInstance  = toJByteArray(env, instance,         impl_->lastError);
    if (!jInitiator || !jContract || !jInstance) {
        if (jInitiator) env->DeleteLocalRef(jInitiator);
        if (jContract)  env->DeleteLocalRef(jContract);
        if (jInstance)  env->DeleteLocalRef(jInstance);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }

    // Class names + bytecodes.
    jclass stringCls = env->FindClass("java/lang/String");
    jobjectArray jnames = env->NewObjectArray(static_cast<jsize>(classNames.size()), stringCls, nullptr);
    for (size_t i = 0; i < classNames.size(); ++i) {
        jstring js = toJString(env, classNames[i], impl_->lastError);
        env->SetObjectArrayElement(jnames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    jclass byteArrCls = env->FindClass("[B");
    jobjectArray jbcs = env->NewObjectArray(static_cast<jsize>(byteCodes.size()), byteArrCls, nullptr);
    for (size_t i = 0; i < byteCodes.size(); ++i) {
        jbyteArray ja = toJByteArray(env, byteCodes[i], impl_->lastError);
        env->SetObjectArrayElement(jbcs, static_cast<jsize>(i), ja);
        env->DeleteLocalRef(ja);
    }

    // Method names + param-group sizes (one per MethodHeader).
    jobjectArray jMethodNames = env->NewObjectArray(static_cast<jsize>(methodNames.size()), stringCls, nullptr);
    for (size_t i = 0; i < methodNames.size(); ++i) {
        jstring js = toJString(env, methodNames[i], impl_->lastError);
        env->SetObjectArrayElement(jMethodNames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    jintArray jGroupSizes = env->NewIntArray(static_cast<jsize>(paramGroupSizes.size()));
    if (!paramGroupSizes.empty()) {
        env->SetIntArrayRegion(jGroupSizes, 0, static_cast<jsize>(paramGroupSizes.size()),
                               reinterpret_cast<const jint*>(paramGroupSizes.data()));
    }

    // Param arrays. Each tag-typed array sized to params.tags.size(); slots
    // unused by the active tag are left zero/null.
    const jsize pn = static_cast<jsize>(paramsFlat.tags.size());
    jintArray    pTags    = env->NewIntArray(pn);
    jbooleanArray pBools  = env->NewBooleanArray(pn);
    jlongArray   pLongs   = env->NewLongArray(pn);
    jdoubleArray pDoubles = env->NewDoubleArray(pn);
    jobjectArray pStrings = env->NewObjectArray(pn, stringCls, nullptr);
    jobjectArray pBytes   = env->NewObjectArray(pn, byteArrCls, nullptr);
    if (pn > 0) {
        env->SetIntArrayRegion(pTags, 0, pn, reinterpret_cast<const jint*>(paramsFlat.tags.data()));
        // bool vector storage isn't contiguous (std::vector<bool>); copy.
        std::vector<jboolean> bb(paramsFlat.bools.size());
        for (size_t i = 0; i < paramsFlat.bools.size(); ++i) bb[i] = paramsFlat.bools[i] ? JNI_TRUE : JNI_FALSE;
        if (!bb.empty()) env->SetBooleanArrayRegion(pBools, 0, static_cast<jsize>(bb.size()), bb.data());
        if (!paramsFlat.longs.empty())   env->SetLongArrayRegion  (pLongs,   0, static_cast<jsize>(paramsFlat.longs.size()),   reinterpret_cast<const jlong*>(paramsFlat.longs.data()));
        if (!paramsFlat.doubles.empty()) env->SetDoubleArrayRegion(pDoubles, 0, static_cast<jsize>(paramsFlat.doubles.size()), paramsFlat.doubles.data());
        for (size_t i = 0; i < paramsFlat.strings.size() && i < static_cast<size_t>(pn); ++i) {
            jstring js = toJString(env, paramsFlat.strings[i], impl_->lastError);
            if (js) {
                env->SetObjectArrayElement(pStrings, static_cast<jsize>(i), js);
                env->DeleteLocalRef(js);
            }
        }
        for (size_t i = 0; i < paramsFlat.bytes.size() && i < static_cast<size_t>(pn); ++i) {
            jbyteArray ja = toJByteArray(env, paramsFlat.bytes[i], impl_->lastError);
            if (ja) {
                env->SetObjectArrayElement(pBytes, static_cast<jsize>(i), ja);
                env->DeleteLocalRef(ja);
            }
        }
    }
    env->DeleteLocalRef(stringCls);
    env->DeleteLocalRef(byteArrCls);

    jobject result = env->CallStaticObjectMethod(
        bridgeCls, mid,
        static_cast<jlong>(accessId),
        jInitiator, jContract,
        jnames, jbcs,
        jInstance, static_cast<jboolean>(stateCanModify ? JNI_TRUE : JNI_FALSE),
        jMethodNames, jGroupSizes,
        pTags, pBools, pLongs, pDoubles, pStrings, pBytes,
        static_cast<jlong>(executionTimeoutMs),
        static_cast<jshort>(version));

    // Cleanup all input local refs.
    env->DeleteLocalRef(jInitiator);
    env->DeleteLocalRef(jContract);
    env->DeleteLocalRef(jInstance);
    env->DeleteLocalRef(jMethodNames);
    env->DeleteLocalRef(jGroupSizes);
    env->DeleteLocalRef(jnames);
    env->DeleteLocalRef(jbcs);
    env->DeleteLocalRef(pTags);
    env->DeleteLocalRef(pBools);
    env->DeleteLocalRef(pLongs);
    env->DeleteLocalRef(pDoubles);
    env->DeleteLocalRef(pStrings);
    env->DeleteLocalRef(pBytes);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "executeByteCodeFull threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "executeByteCodeFull returned null";
        return std::nullopt;
    }

    // Unmarshal the result.
    jclass resCls   = env->GetObjectClass(result);
    jfieldID fCode  = env->GetFieldID(resCls, "code",    "B");
    jfieldID fMsg   = env->GetFieldID(resCls, "message", "Ljava/lang/String;");
    jfieldID fResults = env->GetFieldID(resCls, "results", "[LEmbeddedExecutorBridge$FullSetterResult;");
    if (!fCode || !fMsg || !fResults || env->ExceptionCheck()) {
        impl_->lastError = "GetFieldID failed on FullExecuteResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    FullExecuteResult out;
    out.code = static_cast<int8_t>(env->GetByteField(result, fCode));
    jobject msgObj = env->GetObjectField(result, fMsg);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }

    jobjectArray resultsArr = static_cast<jobjectArray>(env->GetObjectField(result, fResults));
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);

    if (resultsArr) {
        const jsize rn = env->GetArrayLength(resultsArr);
        out.results.reserve(static_cast<size_t>(rn));

        FullSetterResultFieldIds srFields;
        TaggedVariantFieldIds    tvFields;
        FullStateEntryFieldIds   seFields;
        FullEmittedTxnFieldIds   etFields;

        if (rn > 0) {
            jobject probe = env->GetObjectArrayElement(resultsArr, 0);
            srFields = resolveSetterResultFields(env, probe, impl_->lastError);
            if (probe) env->DeleteLocalRef(probe);
            if (!srFields.ok) {
                env->DeleteLocalRef(resultsArr);
                return std::nullopt;
            }
        }

        for (jsize i = 0; i < rn; ++i) {
            jobject elem = env->GetObjectArrayElement(resultsArr, i);
            if (!elem) continue;

            FullSetterResult sr;
            sr.code = static_cast<int8_t>(env->GetByteField(elem, srFields.fCode));
            jobject sMsgObj = env->GetObjectField(elem, srFields.fMsg);
            if (sMsgObj) {
                if (auto s = fromJString(env, static_cast<jstring>(sMsgObj))) sr.message = *s;
                env->DeleteLocalRef(sMsgObj);
            }

            // ret_val: TaggedVariant.
            jobject retObj = env->GetObjectField(elem, srFields.fRetVal);
            if (retObj) {
                if (!tvFields.ok) {
                    tvFields = resolveTaggedVariantFields(env, retObj, impl_->lastError);
                }
                if (tvFields.ok) sr.retVal = readTaggedVariant(env, retObj, tvFields);
                env->DeleteLocalRef(retObj);
            }

            sr.executionCost = static_cast<int64_t>(env->GetLongField(elem, srFields.fCost));

            // contractsState
            jobjectArray stateArr = static_cast<jobjectArray>(env->GetObjectField(elem, srFields.fState));
            if (stateArr) {
                const jsize sn = env->GetArrayLength(stateArr);
                if (sn > 0 && !seFields.ok) {
                    jobject sp = env->GetObjectArrayElement(stateArr, 0);
                    seFields = resolveStateEntryFields(env, sp, impl_->lastError);
                    if (sp) env->DeleteLocalRef(sp);
                }
                sr.contractsState.reserve(static_cast<size_t>(sn));
                for (jsize j = 0; j < sn; ++j) {
                    jobject seElem = env->GetObjectArrayElement(stateArr, j);
                    if (!seElem || !seFields.ok) { if (seElem) env->DeleteLocalRef(seElem); continue; }
                    FullStateEntry se;
                    jbyteArray addr = static_cast<jbyteArray>(env->GetObjectField(seElem, seFields.fAddr));
                    if (addr) {
                        if (auto b = fromJByteArray(env, addr)) se.address = std::move(*b);
                        env->DeleteLocalRef(addr);
                    }
                    jbyteArray st = static_cast<jbyteArray>(env->GetObjectField(seElem, seFields.fState));
                    if (st) {
                        if (auto b = fromJByteArray(env, st)) se.state = std::move(*b);
                        env->DeleteLocalRef(st);
                    }
                    sr.contractsState.push_back(std::move(se));
                    env->DeleteLocalRef(seElem);
                }
                env->DeleteLocalRef(stateArr);
            }

            // emittedTransactions
            jobjectArray emittedArr = static_cast<jobjectArray>(env->GetObjectField(elem, srFields.fEmitted));
            if (emittedArr) {
                const jsize en = env->GetArrayLength(emittedArr);
                if (en > 0 && !etFields.ok) {
                    jobject ep = env->GetObjectArrayElement(emittedArr, 0);
                    etFields = resolveEmittedTxnFields(env, ep, impl_->lastError);
                    if (ep) env->DeleteLocalRef(ep);
                }
                sr.emittedTransactions.reserve(static_cast<size_t>(en));
                for (jsize j = 0; j < en; ++j) {
                    jobject etElem = env->GetObjectArrayElement(emittedArr, j);
                    if (!etElem || !etFields.ok) { if (etElem) env->DeleteLocalRef(etElem); continue; }
                    FullEmittedTxn et;
                    jbyteArray src = static_cast<jbyteArray>(env->GetObjectField(etElem, etFields.fSrc));
                    if (src) {
                        if (auto b = fromJByteArray(env, src)) et.source = std::move(*b);
                        env->DeleteLocalRef(src);
                    }
                    jbyteArray tgt = static_cast<jbyteArray>(env->GetObjectField(etElem, etFields.fTgt));
                    if (tgt) {
                        if (auto b = fromJByteArray(env, tgt)) et.target = std::move(*b);
                        env->DeleteLocalRef(tgt);
                    }
                    et.amountIntegral = static_cast<int32_t>(env->GetIntField(etElem, etFields.fAi));
                    et.amountFraction = static_cast<int64_t>(env->GetLongField(etElem, etFields.fAf));
                    jbyteArray ud = static_cast<jbyteArray>(env->GetObjectField(etElem, etFields.fUd));
                    if (ud) {
                        if (auto b = fromJByteArray(env, ud)) et.userData = std::move(*b);
                        env->DeleteLocalRef(ud);
                    }
                    sr.emittedTransactions.push_back(std::move(et));
                    env->DeleteLocalRef(etElem);
                }
                env->DeleteLocalRef(emittedArr);
            }

            out.results.push_back(std::move(sr));
            env->DeleteLocalRef(elem);
        }
        if (srFields.cls) env->DeleteLocalRef(srFields.cls);
        if (tvFields.cls) env->DeleteLocalRef(tvFields.cls);
        if (seFields.cls) env->DeleteLocalRef(seFields.cls);
        if (etFields.cls) env->DeleteLocalRef(etFields.cls);
        env->DeleteLocalRef(resultsArr);
    }

    impl_->lastError.clear();
    return out;
}

// ----- callExecuteByteCodeMultipleFull ---------------------------------------
// Forwards to executeByteCodeMultipleFull on the bridge with a flat-matrix
// representation of the params. Result shape is FullExecuteResult; Multi
// returns one FullSetterResult per call (each with retVal but no state/emitted
// — those don't apply to the multiple-getter API).

std::optional<Vm::FullExecuteResult>
Vm::callExecuteByteCodeMultipleFull(int64_t                                  accessId,
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
                                    int16_t                                  version) {
    if (!impl_->started) { impl_->lastError = "VM not started"; return std::nullopt; }
    if (classNames.size() != byteCodes.size()) {
        impl_->lastError = "classNames / byteCodes length mismatch"; return std::nullopt;
    }
    ThreadGuard guard(*this);
    if (!guard.ok()) { impl_->lastError = "thread attach failed"; return std::nullopt; }
    JNIEnv* env = guard.env();
    LocalFrame frame(env, 256);
    if (!frame.active) { impl_->lastError = "PushLocalFrame failed"; return std::nullopt; }

    jclass bridgeCls = findClassChecked(env, "EmbeddedExecutorBridge", impl_->lastError);
    if (!bridgeCls) return std::nullopt;
    jmethodID mid = findStaticMethodChecked(
        env, bridgeCls, "executeByteCodeMultipleFull",
        "(J[B[B[Ljava/lang/String;[[B[BZLjava/lang/String;[I[I[Z[J[D[Ljava/lang/String;[[BJS)"
        "LEmbeddedExecutorBridge$FullExecuteResult;",
        impl_->lastError);
    if (!mid) { env->DeleteLocalRef(bridgeCls); return std::nullopt; }

    jbyteArray jInitiator = toJByteArray(env, initiatorAddress, impl_->lastError);
    jbyteArray jContract  = toJByteArray(env, contractAddress,  impl_->lastError);
    jbyteArray jInstance  = toJByteArray(env, instance,         impl_->lastError);
    jstring    jMethod    = toJString  (env, methodName,        impl_->lastError);
    if (!jInitiator || !jContract || !jInstance || !jMethod) {
        if (jInitiator) env->DeleteLocalRef(jInitiator);
        if (jContract)  env->DeleteLocalRef(jContract);
        if (jInstance)  env->DeleteLocalRef(jInstance);
        if (jMethod)    env->DeleteLocalRef(jMethod);
        env->DeleteLocalRef(bridgeCls);
        return std::nullopt;
    }

    jclass stringCls = env->FindClass("java/lang/String");
    jobjectArray jnames = env->NewObjectArray(static_cast<jsize>(classNames.size()), stringCls, nullptr);
    for (size_t i = 0; i < classNames.size(); ++i) {
        jstring js = toJString(env, classNames[i], impl_->lastError);
        env->SetObjectArrayElement(jnames, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    jclass byteArrCls = env->FindClass("[B");
    jobjectArray jbcs = env->NewObjectArray(static_cast<jsize>(byteCodes.size()), byteArrCls, nullptr);
    for (size_t i = 0; i < byteCodes.size(); ++i) {
        jbyteArray ja = toJByteArray(env, byteCodes[i], impl_->lastError);
        env->SetObjectArrayElement(jbcs, static_cast<jsize>(i), ja);
        env->DeleteLocalRef(ja);
    }

    // paramGroupSizes
    jintArray jGroupSizes = env->NewIntArray(static_cast<jsize>(paramGroupSizes.size()));
    if (!paramGroupSizes.empty()) {
        env->SetIntArrayRegion(jGroupSizes, 0, static_cast<jsize>(paramGroupSizes.size()),
                               reinterpret_cast<const jint*>(paramGroupSizes.data()));
    }

    // Flat param arrays.
    const jsize pn = static_cast<jsize>(paramsFlat.tags.size());
    jintArray    pTags    = env->NewIntArray(pn);
    jbooleanArray pBools  = env->NewBooleanArray(pn);
    jlongArray   pLongs   = env->NewLongArray(pn);
    jdoubleArray pDoubles = env->NewDoubleArray(pn);
    jobjectArray pStrings = env->NewObjectArray(pn, stringCls, nullptr);
    jobjectArray pBytes   = env->NewObjectArray(pn, byteArrCls, nullptr);
    if (pn > 0) {
        env->SetIntArrayRegion(pTags, 0, pn, reinterpret_cast<const jint*>(paramsFlat.tags.data()));
        std::vector<jboolean> bb(paramsFlat.bools.size());
        for (size_t i = 0; i < paramsFlat.bools.size(); ++i) bb[i] = paramsFlat.bools[i] ? JNI_TRUE : JNI_FALSE;
        if (!bb.empty()) env->SetBooleanArrayRegion(pBools, 0, static_cast<jsize>(bb.size()), bb.data());
        if (!paramsFlat.longs.empty())   env->SetLongArrayRegion  (pLongs,   0, static_cast<jsize>(paramsFlat.longs.size()),   reinterpret_cast<const jlong*>(paramsFlat.longs.data()));
        if (!paramsFlat.doubles.empty()) env->SetDoubleArrayRegion(pDoubles, 0, static_cast<jsize>(paramsFlat.doubles.size()), paramsFlat.doubles.data());
        for (size_t i = 0; i < paramsFlat.strings.size() && i < static_cast<size_t>(pn); ++i) {
            jstring js = toJString(env, paramsFlat.strings[i], impl_->lastError);
            if (js) { env->SetObjectArrayElement(pStrings, static_cast<jsize>(i), js); env->DeleteLocalRef(js); }
        }
        for (size_t i = 0; i < paramsFlat.bytes.size() && i < static_cast<size_t>(pn); ++i) {
            jbyteArray ja = toJByteArray(env, paramsFlat.bytes[i], impl_->lastError);
            if (ja) { env->SetObjectArrayElement(pBytes, static_cast<jsize>(i), ja); env->DeleteLocalRef(ja); }
        }
    }
    env->DeleteLocalRef(stringCls);
    env->DeleteLocalRef(byteArrCls);

    jobject result = env->CallStaticObjectMethod(
        bridgeCls, mid,
        static_cast<jlong>(accessId),
        jInitiator, jContract,
        jnames, jbcs,
        jInstance, static_cast<jboolean>(stateCanModify ? JNI_TRUE : JNI_FALSE),
        jMethod,
        jGroupSizes,
        pTags, pBools, pLongs, pDoubles, pStrings, pBytes,
        static_cast<jlong>(executionTimeoutMs),
        static_cast<jshort>(version));

    env->DeleteLocalRef(jInitiator);
    env->DeleteLocalRef(jContract);
    env->DeleteLocalRef(jInstance);
    env->DeleteLocalRef(jMethod);
    env->DeleteLocalRef(jnames);
    env->DeleteLocalRef(jbcs);
    env->DeleteLocalRef(jGroupSizes);
    env->DeleteLocalRef(pTags);
    env->DeleteLocalRef(pBools);
    env->DeleteLocalRef(pLongs);
    env->DeleteLocalRef(pDoubles);
    env->DeleteLocalRef(pStrings);
    env->DeleteLocalRef(pBytes);
    env->DeleteLocalRef(bridgeCls);

    if (env->ExceptionCheck()) {
        impl_->lastError = "executeByteCodeMultipleFull threw: " + drainPendingException(env);
        return std::nullopt;
    }
    if (!result) {
        impl_->lastError = "executeByteCodeMultipleFull returned null";
        return std::nullopt;
    }

    // Reuse the same FullExecuteResult unmarshalling as the single-method
    // path. Inline it here rather than factor out (the original uses lots of
    // local lambdas/state which would complicate the helper).
    jclass resCls   = env->GetObjectClass(result);
    jfieldID fCode  = env->GetFieldID(resCls, "code",    "B");
    jfieldID fMsg   = env->GetFieldID(resCls, "message", "Ljava/lang/String;");
    jfieldID fResults = env->GetFieldID(resCls, "results", "[LEmbeddedExecutorBridge$FullSetterResult;");
    if (!fCode || !fMsg || !fResults) {
        impl_->lastError = "GetFieldID failed on FullExecuteResult: " + drainPendingException(env);
        env->DeleteLocalRef(resCls);
        env->DeleteLocalRef(result);
        return std::nullopt;
    }

    FullExecuteResult out;
    out.code = static_cast<int8_t>(env->GetByteField(result, fCode));
    jobject msgObj = env->GetObjectField(result, fMsg);
    if (msgObj) {
        if (auto s = fromJString(env, static_cast<jstring>(msgObj))) out.message = *s;
        env->DeleteLocalRef(msgObj);
    }

    jobjectArray resultsArr = static_cast<jobjectArray>(env->GetObjectField(result, fResults));
    env->DeleteLocalRef(resCls);
    env->DeleteLocalRef(result);

    if (resultsArr) {
        const jsize rn = env->GetArrayLength(resultsArr);
        out.results.reserve(static_cast<size_t>(rn));
        FullSetterResultFieldIds srFields;
        TaggedVariantFieldIds    tvFields;
        if (rn > 0) {
            jobject probe = env->GetObjectArrayElement(resultsArr, 0);
            srFields = resolveSetterResultFields(env, probe, impl_->lastError);
            if (probe) env->DeleteLocalRef(probe);
            if (!srFields.ok) { env->DeleteLocalRef(resultsArr); return std::nullopt; }
        }
        for (jsize i = 0; i < rn; ++i) {
            jobject elem = env->GetObjectArrayElement(resultsArr, i);
            if (!elem) continue;
            FullSetterResult sr;
            sr.code = static_cast<int8_t>(env->GetByteField(elem, srFields.fCode));
            jobject sMsgObj = env->GetObjectField(elem, srFields.fMsg);
            if (sMsgObj) {
                if (auto s = fromJString(env, static_cast<jstring>(sMsgObj))) sr.message = *s;
                env->DeleteLocalRef(sMsgObj);
            }
            jobject retObj = env->GetObjectField(elem, srFields.fRetVal);
            if (retObj) {
                if (!tvFields.ok) tvFields = resolveTaggedVariantFields(env, retObj, impl_->lastError);
                if (tvFields.ok) sr.retVal = readTaggedVariant(env, retObj, tvFields);
                env->DeleteLocalRef(retObj);
            }
            sr.executionCost = static_cast<int64_t>(env->GetLongField(elem, srFields.fCost));
            // Multiple results don't carry contractsState / emitted txns —
            // GetterMethodResult on the Java side doesn't have those fields.
            out.results.push_back(std::move(sr));
            env->DeleteLocalRef(elem);
        }
        if (srFields.cls) env->DeleteLocalRef(srFields.cls);
        if (tvFields.cls) env->DeleteLocalRef(tvFields.cls);
        env->DeleteLocalRef(resultsArr);
    }

    impl_->lastError.clear();
    return out;
}
}  // namespace jvm_embed
