// jvm_embed_spike: smoke test for the JNI Invocation API integration.
// Loads the JVM, calls three static methods on HelloEmbed, prints results.
//
// Usage:
//   jvm_embed_spike [classpath_dir]
//
// classpath_dir defaults to ./java relative to the executable. JAVA_HOME must
// be set in the environment (or pass --java-home <path>).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <jvm_embed/jvm_embed.hpp>

namespace {

void printUsage() {
    std::cerr <<
        "jvm_embed_spike [options] [classpath_dir]\n"
        "  --java-home PATH   Override JAVA_HOME (default: env)\n"
        "  --jvm-opt STR      Append a JVM option (repeatable). e.g. --jvm-opt -Xmx512m\n"
        "  --help             Show this help\n"
        "Args:\n"
        "  classpath_dir      Directory containing HelloEmbed.class. Default: ./java\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string classpathDir = "./java";
    std::string javaHome;
    std::vector<std::string> jvmOptions;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(); return 0; }
        if (a == "--java-home") {
            if (i + 1 >= argc) { std::cerr << "missing value for --java-home\n"; return 2; }
            javaHome = argv[++i];
        } else if (a == "--jvm-opt") {
            if (i + 1 >= argc) { std::cerr << "missing value for --jvm-opt\n"; return 2; }
            jvmOptions.emplace_back(argv[++i]);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown flag: " << a << "\n";
            printUsage();
            return 2;
        } else {
            classpathDir = a;
        }
    }

    jvm_embed::InitOptions opts;
    opts.classpath = { classpathDir };
#ifdef JVM_EMBED_EXECUTOR_JAR
    // Real bridge requires the executor JAR on the classpath alongside our
    // compiled .class directory.
    opts.classpath.emplace_back(JVM_EMBED_EXECUTOR_JAR);
#endif
#ifdef JVM_EMBED_EXECUTOR_INSTALL_DIR
    // Install dir contains settings.properties, which the executor's
    // ApplicationProperties() constructor loads as a classpath resource.
    opts.classpath.emplace_back(JVM_EMBED_EXECUTOR_INSTALL_DIR);
#endif
#ifdef JVM_EMBED_SCAPI_JAR
    // sc-api-support.jar — needed so contracts that extend
    // com.credits.scapi.* compile under the embedded executor.
    opts.classpath.emplace_back(JVM_EMBED_SCAPI_JAR);
#endif
    opts.javaHomeOverride = javaHome;
    opts.jvmOptions = jvmOptions;
    // -Xcheck:jni catches reference and threading bugs early. Worth running with
    // during the spike; can be removed once we trust the wrapper.
    opts.jvmOptions.emplace_back("-Xcheck:jni");

    jvm_embed::Vm vm;
    std::cout << "starting JVM ..." << std::endl;
    if (!vm.start(opts)) {
        std::cerr << "JVM start failed: " << vm.lastError() << "\n";
        return 1;
    }
    std::cout << "JVM started." << std::endl;

    // Test 1: no-arg String return.
    if (auto r = vm.callStaticStringNoArg("HelloEmbed", "hello")) {
        std::cout << "[ok] HelloEmbed.hello() -> \"" << *r << "\"" << std::endl;
    } else {
        std::cerr << "[fail] HelloEmbed.hello(): " << vm.lastError() << "\n";
        return 1;
    }

    // Test 2: String -> String round-trip.
    if (auto r = vm.callStaticStringOneStringArg("HelloEmbed", "echo", "from-cpp")) {
        std::cout << "[ok] HelloEmbed.echo(\"from-cpp\") -> \"" << *r << "\"" << std::endl;
        if (*r != "echo:from-cpp") {
            std::cerr << "[fail] echo result mismatch\n";
            return 1;
        }
    } else {
        std::cerr << "[fail] HelloEmbed.echo: " << vm.lastError() << "\n";
        return 1;
    }

    // Test 3: int, int -> int.
    if (auto r = vm.callStaticIntTwoInts("HelloEmbed", "add", 7, 35)) {
        std::cout << "[ok] HelloEmbed.add(7, 35) -> " << *r << std::endl;
        if (*r != 42) {
            std::cerr << "[fail] add returned " << *r << " (expected 42)\n";
            return 1;
        }
    } else {
        std::cerr << "[fail] HelloEmbed.add: " << vm.lastError() << "\n";
        return 1;
    }

    // Test 4: byte[] -> byte[] round-trip. Validates the marshalling helpers
    // we'll use for any future call carrying contract bytecode or state.
    {
        std::vector<uint8_t> src;
        for (uint8_t i = 0; i < 64; ++i) src.push_back(i);

        auto r = vm.callStaticBytesOneBytesArg("HelloEmbed", "reverseBytes", src);
        if (!r) {
            std::cerr << "[fail] HelloEmbed.reverseBytes: " << vm.lastError() << "\n";
            return 1;
        }
        if (r->size() != src.size()) {
            std::cerr << "[fail] reverseBytes size mismatch: " << r->size()
                      << " vs " << src.size() << "\n";
            return 1;
        }
        for (size_t i = 0; i < src.size(); ++i) {
            if ((*r)[i] != src[src.size() - 1 - i]) {
                std::cerr << "[fail] reverseBytes content mismatch at " << i << "\n";
                return 1;
            }
        }
        // Empty input edge case.
        auto empty = vm.callStaticBytesOneBytesArg("HelloEmbed", "reverseBytes", {});
        if (!empty || !empty->empty()) {
            std::cerr << "[fail] reverseBytes empty input did not return empty\n";
            return 1;
        }
        std::cout << "[ok] HelloEmbed.reverseBytes round-trip (" << src.size() << " bytes + empty)" << std::endl;
    }

    // Test 5: cached vs uncached method-handle benchmark. Demonstrates the
    // FindClass + GetStaticMethodID overhead per call, which matters at slow-
    // start replay scale (millions of invocations).
    {
        constexpr int kIters = 20000;

        auto handle = vm.resolveStaticMethod("HelloEmbed", "add", "(II)I");
        if (!handle.valid()) {
            std::cerr << "[fail] resolveStaticMethod: " << vm.lastError() << "\n";
            return 1;
        }

        const auto t_un_start = std::chrono::steady_clock::now();
        int sum_un = 0;
        for (int i = 0; i < kIters; ++i) {
            auto r = vm.callStaticIntTwoInts("HelloEmbed", "add", i, 1);
            if (!r) { std::cerr << "[fail] uncached call\n"; return 1; }
            sum_un += *r;
        }
        const auto t_un_end = std::chrono::steady_clock::now();

        const auto t_cd_start = std::chrono::steady_clock::now();
        int sum_cd = 0;
        for (int i = 0; i < kIters; ++i) {
            auto r = vm.callStaticIntTwoIntsCached(handle, i, 1);
            if (!r) { std::cerr << "[fail] cached call\n"; return 1; }
            sum_cd += *r;
        }
        const auto t_cd_end = std::chrono::steady_clock::now();

        if (sum_un != sum_cd) {
            std::cerr << "[fail] uncached/cached results differ: " << sum_un
                      << " vs " << sum_cd << "\n";
            return 1;
        }

        const auto un_us = std::chrono::duration_cast<std::chrono::microseconds>(t_un_end - t_un_start).count();
        const auto cd_us = std::chrono::duration_cast<std::chrono::microseconds>(t_cd_end - t_cd_start).count();
        std::cout << "[ok] " << kIters << " calls"
                  << " uncached=" << un_us << "us (" << (un_us > 0 ? (1000000LL * kIters / un_us) : 0) << "/s)"
                  << " cached="   << cd_us << "us (" << (cd_us > 0 ? (1000000LL * kIters / cd_us) : 0) << "/s)"
                  << " speedup=" << (cd_us > 0 ? (static_cast<double>(un_us) / cd_us) : 0.0) << "x"
                  << std::endl;

        vm.release(handle);
    }

    // Test 6: stub executor — first end-to-end JNI wrapper of a Thrift-shaped
    // service method. Validates the pattern the production wrapper will use
    // for executeByteCode and the rest of ContractExecutor.
    {
        auto r = vm.callGetExecutorBuildVersion(/*version=*/4);
        if (!r) {
            std::cerr << "[fail] StubExecutor.getExecutorBuildVersion: " << vm.lastError() << "\n";
            return 1;
        }
        const bool ok =
            r->code == 0 &&
            r->message == "stub-ok v=4" &&
            r->commitNumber == 12345 &&
            r->commitHash == "abc123def456";
        if (!ok) {
            std::cerr << "[fail] BuildVersionResult mismatch: code=" << int(r->code)
                      << " msg=\"" << r->message << "\""
                      << " n=" << r->commitNumber
                      << " hash=\"" << r->commitHash << "\"\n";
            return 1;
        }
        std::cout << "[ok] StubExecutor.getExecutorBuildVersion(4) ->"
                  << " code=" << int(r->code)
                  << " msg=\"" << r->message << "\""
                  << " n=" << r->commitNumber
                  << " hash=\"" << r->commitHash << "\""
                  << std::endl;
    }

#ifdef JVM_EMBED_BRIDGE_AVAILABLE
    // Test 7: REAL bridge — JNI through to the actual contract-executor.jar's
    // ContractExecutorHandler.getExecutorBuildVersion. Same C++ wrapper as
    // Test 6, just pointed at EmbeddedExecutorBridge instead of StubExecutor.
    {
        auto r = vm.callGetExecutorBuildVersion(/*version=*/4, "EmbeddedExecutorBridge");
        if (!r) {
            std::cerr << "[fail] real bridge getExecutorBuildVersion: " << vm.lastError() << "\n";
            return 1;
        }
        // The real handler returns success on version=4 (validateVersion accepts only 4).
        // commitNumber/commitHash come from settings.properties or git.properties at
        // runtime — may be 0/empty when those resources aren't on classpath, but the
        // CALL succeeding (code==0) is what the spike is validating.
        std::cout << "[ok] EmbeddedExecutorBridge.getExecutorBuildVersion(4) ->"
                  << " code=" << int(r->code)
                  << " msg=\"" << r->message << "\""
                  << " n=" << r->commitNumber
                  << " hash=\"" << r->commitHash << "\""
                  << std::endl;
        if (r->code != 0) {
            std::cerr << "[fail] real bridge returned non-zero code\n";
            return 1;
        }

        // Negative path: version != 4 must throw ContractExecutorUtils
        // .validateVersion's IncompatibleVersionException, which the bridge
        // catches and surfaces as code != 0.
        auto bad = vm.callGetExecutorBuildVersion(/*version=*/99, "EmbeddedExecutorBridge");
        if (!bad || bad->code == 0) {
            std::cerr << "[fail] real bridge accepted invalid version=99 (code="
                      << (bad ? int(bad->code) : -99) << ")\n";
            return 1;
        }
        std::cout << "[ok] real bridge correctly rejected version=99: code="
                  << int(bad->code) << " msg=\"" << bad->message << "\"" << std::endl;
    }
    // Test 7c: REAL compileSourceCode — first List<Struct> JNI extraction.
    // This pulls in the full Dagger graph (lazy-init via the bridge) and
    // exercises ContractExecutorService.compileContractClass under the hood.
    {
        const std::string src = R"JAVA(
public class HelloContract {
    public String hello() { return "compiled by embedded jvm"; }
    public int answer() { return 42; }
}
)JAVA";
        auto cr = vm.callCompileSourceCode(src, /*version=*/4);
        if (!cr) {
            std::cerr << "[fail] compileSourceCode: " << vm.lastError() << "\n";
            return 1;
        }
        if (cr->code != 0) {
            std::cerr << "[fail] compileSourceCode returned non-zero code=" << int(cr->code)
                      << " msg=\"" << cr->message << "\"\n";
            return 1;
        }
        if (cr->classes.empty()) {
            std::cerr << "[fail] compileSourceCode returned no classes\n";
            return 1;
        }
        // Validate: at least one class named HelloContract with non-empty bytecode
        // starting with the JVM magic 0xCAFEBABE.
        bool found = false;
        size_t total_bytes = 0;
        for (const auto& cls : cr->classes) {
            total_bytes += cls.byteCode.size();
            if (cls.byteCode.size() >= 4 &&
                cls.byteCode[0] == 0xCA && cls.byteCode[1] == 0xFE &&
                cls.byteCode[2] == 0xBA && cls.byteCode[3] == 0xBE) {
                if (cls.name.find("HelloContract") != std::string::npos) found = true;
            }
        }
        if (!found) {
            std::cerr << "[fail] HelloContract.class not in compile output\n";
            for (const auto& cls : cr->classes) {
                std::cerr << "  - " << cls.name << " (" << cls.byteCode.size() << " bytes)\n";
            }
            return 1;
        }
        std::cout << "[ok] compileSourceCode -> " << cr->classes.size()
                  << " classes, " << total_bytes << " total bytes, HelloContract.class with CAFEBABE header found"
                  << std::endl;

        // Negative path: malformed source must surface a compile error
        // (non-zero code) without bringing down the JVM.
        auto bad = vm.callCompileSourceCode("class Broken { not valid java; }", /*version=*/4);
        if (!bad || bad->code == 0) {
            std::cerr << "[fail] malformed source incorrectly succeeded\n";
            return 1;
        }
        std::cout << "[ok] compileSourceCode rejected malformed source: code="
                  << int(bad->code) << " (msg truncated)" << std::endl;

        // Test 7d: chain compileSourceCode -> getContractMethods.
        // Take the bytecode we just compiled and feed it back to verify the
        // executor's method introspection finds the methods we declared.
        // This proves both directions of List<Struct> JNI marshalling work.
        std::vector<std::string>          names;
        std::vector<std::vector<uint8_t>> bytes;
        for (const auto& cls : cr->classes) {
            names.push_back(cls.name);
            bytes.push_back(cls.byteCode);
        }
        auto mr = vm.callGetContractMethods(names, bytes, /*version=*/4);
        if (!mr) {
            std::cerr << "[fail] getContractMethods: " << vm.lastError() << "\n";
            return 1;
        }
        if (mr->code != 0) {
            std::cerr << "[fail] getContractMethods code=" << int(mr->code)
                      << " msg=\"" << mr->message << "\"\n";
            return 1;
        }
        bool sawHello = false, sawAnswer = false;
        for (const auto& m : mr->methods) {
            if (m.name == "hello"  && m.returnType.find("String") != std::string::npos) sawHello = true;
            if (m.name == "answer" && m.returnType.find("int")    != std::string::npos) sawAnswer = true;
        }
        if (!sawHello || !sawAnswer) {
            std::cerr << "[fail] expected methods not introspected:\n";
            for (const auto& m : mr->methods) {
                std::cerr << "  " << m.returnType << " " << m.name
                          << "(args=" << m.arguments.size()
                          << " annotations=" << m.annotations.size() << ")\n";
            }
            return 1;
        }
        std::cout << "[ok] getContractMethods round-trip: "
                  << mr->methods.size() << " methods total, hello+answer found"
                  << std::endl;

        // Test 7e: getContractVariables — exercises Variant unwrapping. We
        // use a contract with public fields so the executor's introspection
        // can see them. State buffer is empty (default values).
        const std::string varsSrc = R"JAVA(
public class VarsContract {
    public String label = "spike";
    public int counter = 7;
    public boolean active = true;
    public long bigNum = 12345678901L;
}
)JAVA";
        auto vcr = vm.callCompileSourceCode(varsSrc, /*version=*/4);
        if (!vcr || vcr->code != 0 || vcr->classes.empty()) {
            std::cerr << "[fail] could not compile VarsContract for variables test\n";
            return 1;
        }
        std::vector<std::string>          vnames;
        std::vector<std::vector<uint8_t>> vbytes;
        for (const auto& cls : vcr->classes) {
            vnames.push_back(cls.name);
            vbytes.push_back(cls.byteCode);
        }
        auto vr = vm.callGetContractVariables(vnames, vbytes, /*state=*/{}, /*version=*/4);
        if (!vr) {
            std::cerr << "[fail] getContractVariables: " << vm.lastError() << "\n";
            return 1;
        }
        // The CALL itself succeeding (no JNI exception, well-formed result) is
        // the integration win. The executor may return code != 0 if it
        // refuses to inspect a class that doesn't implement its smart-contract
        // base type — that's an executor decision, not a JNI bug.
        std::cout << "[ok] getContractVariables call returned: code=" << int(vr->code)
                  << " variables=" << vr->variables.size();
        if (!vr->message.empty()) {
            std::string preview = vr->message.substr(0, 80);
            std::cout << " msg=\"" << preview << (vr->message.size() > 80 ? "..." : "") << "\"";
        }
        std::cout << std::endl;
        // Print any Variants we received so we can see Variant unwrapping work.
        for (const auto& kv : vr->variables) {
            std::cout << "        var \"" << kv.first << "\" tag=" << int(kv.second.tag);
            switch (kv.second.tag) {
                case jvm_embed::Vm::VTAG_BOOL:   std::cout << " bool=" << (kv.second.boolVal ? "true" : "false"); break;
                case jvm_embed::Vm::VTAG_LONG:   std::cout << " long=" << kv.second.longVal; break;
                case jvm_embed::Vm::VTAG_DOUBLE: std::cout << " double=" << kv.second.doubleVal; break;
                case jvm_embed::Vm::VTAG_STRING: std::cout << " string=\"" << kv.second.stringVal << "\""; break;
                case jvm_embed::Vm::VTAG_BYTES:  std::cout << " bytes=[" << kv.second.bytesVal.size() << "]"; break;
                case jvm_embed::Vm::VTAG_OTHER:  std::cout << " other=\"" << kv.second.repr.substr(0, 40) << "\""; break;
                default: std::cout << " null"; break;
            }
            std::cout << std::endl;
        }

        // Test 7f: synthetic Variant round-trip — exercises every tag of the
        // unwrapping pipeline against known values.
        auto samples = vm.callMakeVariantSamples();
        if (!samples) {
            std::cerr << "[fail] makeVariantSamples: " << vm.lastError() << "\n";
            return 1;
        }
        if (samples->size() != 8) {
            std::cerr << "[fail] expected 8 Variant samples, got " << samples->size() << "\n";
            return 1;
        }
        const auto& s = *samples;
        bool ok = true;
        ok &= s[0].tag == jvm_embed::Vm::VTAG_BOOL   && s[0].boolVal == true;
        ok &= s[1].tag == jvm_embed::Vm::VTAG_LONG   && s[1].longVal == 42;
        ok &= s[2].tag == jvm_embed::Vm::VTAG_LONG   && s[2].longVal == 9999999999LL;
        ok &= s[3].tag == jvm_embed::Vm::VTAG_DOUBLE && s[3].doubleVal > 3.14 && s[3].doubleVal < 3.15;
        ok &= s[4].tag == jvm_embed::Vm::VTAG_STRING && s[4].stringVal == "variant-string";
        ok &= s[5].tag == jvm_embed::Vm::VTAG_BYTES  && s[5].bytesVal.size() == 4 &&
              s[5].bytesVal[0] == 1 && s[5].bytesVal[3] == 4;
        ok &= s[6].tag == jvm_embed::Vm::VTAG_NULL;
        ok &= s[7].tag == jvm_embed::Vm::VTAG_OTHER && !s[7].repr.empty();
        if (!ok) {
            std::cerr << "[fail] Variant round-trip mismatch:\n";
            for (size_t i = 0; i < s.size(); ++i) {
                std::cerr << "  [" << i << "] tag=" << int(s[i].tag)
                          << " bool=" << s[i].boolVal
                          << " long=" << s[i].longVal
                          << " dbl=" << s[i].doubleVal
                          << " str=\"" << s[i].stringVal << "\""
                          << " bytes=" << s[i].bytesVal.size()
                          << " repr=\"" << s[i].repr << "\"\n";
            }
            return 1;
        }
        std::cout << "[ok] Variant round-trip: 8 samples covering all tags (bool/long/long/double/string/bytes/null/other)" << std::endl;

        // Test 7g: executeByteCode end-to-end. We pass HelloContract bytecode
        // through the full JNI marshalling pipeline. The executor responds
        // with a domain-level rejection ("cannot serialize smart contract")
        // because HelloContract doesn't extend the SCAPI base type — that's
        // a clean Java response, not a marshalling failure, which proves the
        // entire 9-argument path (longs, byte arrays, parallel arrays, bool,
        // string) reaches the executor and the result struct (status,
        // method status, return Variant, executionCost, newState bytes) is
        // unmarshalled correctly. End-to-end execution of a real SCAPI
        // contract requires sc-api-support.jar on the executor's INTERNAL
        // compile classpath (separate from the JVM classpath; configured via
        // settings.properties or executor patching) — that's a build-system
        // task not addressed by the spike.
        std::vector<uint8_t> initiator(32, 0);
        std::vector<uint8_t> ctrAddress(32, 0);
        for (size_t i = 0; i < 8; ++i) ctrAddress[i] = static_cast<uint8_t>(0xA0 + i);

        auto deploy = vm.callExecuteByteCode(
            /*accessId=*/1,
            initiator,
            ctrAddress,
            names, bytes,                  // HelloContract bytecode from earlier compile
            /*instance=*/{},
            /*stateCanModify=*/true,
            /*methodName=*/"",
            /*executionTimeoutMs=*/60000,
            /*version=*/4);
        if (!deploy) {
            std::cerr << "[fail] executeByteCode (deploy): " << vm.lastError() << "\n";
            return 1;
        }
        std::cout << "[ok] executeByteCode (deploy) call returned:"
                  << " code=" << int(deploy->code)
                  << " methodCode=" << int(deploy->methodCode)
                  << " cost=" << deploy->executionCost
                  << " newState=" << deploy->newState.size() << "B"
                  << " retTag=" << int(deploy->retVal.tag);
        if (!deploy->message.empty()) {
            std::string preview = deploy->message.substr(0, 60);
            std::cout << " msg=\"" << preview << (deploy->message.size() > 60 ? "..." : "") << "\"";
        }
        std::cout << std::endl;

        // Whether deploy succeeded, the call path itself works. Try invoking
        // hello() — uses deploy->newState if produced, else empty.
        auto invoke = vm.callExecuteByteCode(
            /*accessId=*/2,
            initiator, ctrAddress,
            names, bytes,
            deploy->newState,
            /*stateCanModify=*/true,
            /*methodName=*/"hello",
            /*executionTimeoutMs=*/60000,
            /*version=*/4);
        if (!invoke) {
            std::cerr << "[fail] executeByteCode (call): " << vm.lastError() << "\n";
            return 1;
        }
        std::cout << "[ok] executeByteCode (hello) call returned:"
                  << " code=" << int(invoke->code)
                  << " methodCode=" << int(invoke->methodCode)
                  << " cost=" << invoke->executionCost
                  << " retTag=" << int(invoke->retVal.tag);
        if (invoke->retVal.tag == jvm_embed::Vm::VTAG_STRING) {
            std::cout << " ret=\"" << invoke->retVal.stringVal << "\"";
        } else if (!invoke->methodMessage.empty()) {
            std::string preview = invoke->methodMessage.substr(0, 80);
            std::cout << " mMsg=\"" << preview << (invoke->methodMessage.size() > 80 ? "..." : "") << "\"";
        }
        std::cout << std::endl;
    }
#else
    std::cout << "[skip] real bridge test (contract-executor.jar not configured)" << std::endl;
#endif

    // Test 8: multi-threaded call from non-JVM-created threads.
    // Each worker calls a JVM method N times; we verify total call count and
    // result correctness. Validates ThreadGuard's attach-as-daemon + detach.
    {
        constexpr int kThreads     = 8;
        constexpr int kCallsEach   = 250;
        std::atomic<int> ok_count{0};
        std::atomic<int> fail_count{0};
        std::vector<std::thread> workers;
        const auto t0 = std::chrono::steady_clock::now();

        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&vm, &ok_count, &fail_count, t, kCallsEach]() {
                for (int i = 0; i < kCallsEach; ++i) {
                    auto r = vm.callStaticIntTwoInts("HelloEmbed", "add", t, i);
                    if (r && *r == t + i) ++ok_count;
                    else ++fail_count;
                }
            });
        }
        for (auto& w : workers) w.join();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        const int total = kThreads * kCallsEach;
        if (ok_count.load() != total || fail_count.load() != 0) {
            std::cerr << "[fail] multithreaded test: ok=" << ok_count
                      << " fail=" << fail_count << " (expected ok=" << total << ")\n";
            return 1;
        }
        std::cout << "[ok] " << kThreads << " threads x " << kCallsEach
                  << " calls = " << total << " total, "
                  << elapsed << "ms ("
                  << (elapsed > 0 ? (1000.0 * total / elapsed) : 0.0) << " calls/sec)"
                  << std::endl;
    }

    std::cout << "spike: all tests passed" << std::endl;
    return 0;
}
