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

}  // namespace jvm_embed
