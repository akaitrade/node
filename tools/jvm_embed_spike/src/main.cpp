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

    // Test 7: multi-threaded call from non-JVM-created threads.
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
