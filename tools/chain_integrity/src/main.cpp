// chain_integrity: walk a CREDITS chain DB and verify hash chain + sequence monotonicity.
// Read-only; operates directly on the underlying database backend (no Storage::open).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <csdb/database.hpp>
#include <csnode/chain_integrity.hpp>

#if defined(CSDB_USE_ROCKSDB)
#include <csdb/database_rocksdb.hpp>
#endif
#if defined(CSDB_USE_BERKELEYDB)
#include <csdb/database_berkeleydb.hpp>
#endif

namespace {

struct Args {
    std::string path;
    std::string backend = "rocksdb";
    cs::Sequence from = 0;
    cs::Sequence to = std::numeric_limits<cs::Sequence>::max();
    size_t progress_every = 100000;
    size_t mismatch_print_limit = 50;
};

void print_usage() {
    std::cerr <<
        "chain_integrity --path DB [options]\n"
        "  --path PATH      Chain DB directory\n"
        "  --backend X      rocksdb (default) or berkeleydb\n"
        "  --from N         Start sequence (default 0)\n"
        "  --to N           End sequence inclusive (default: end of DB)\n"
        "  --progress N     Progress log interval (default 100000)\n"
        "  --max-print N    First N mismatches to print in detail (default 50)\n"
        "\n"
        "Walks the chain DB by sequence. For each block: verifies\n"
        "prev_hash == previousBlock.hash() and sequence == prev + 1.\n"
        "Reports gaps, prev-hash mismatches, decode failures, missing pools.\n";
}

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (++i >= argc) {
                std::cerr << "missing value for " << what << "\n";
                std::exit(2);
            }
            return argv[i];
        };
        if (a == "--path") out.path = next("--path");
        else if (a == "--backend") out.backend = next("--backend");
        else if (a == "--from") out.from = std::stoull(next("--from"));
        else if (a == "--to") out.to = std::stoull(next("--to"));
        else if (a == "--progress") out.progress_every = std::stoull(next("--progress"));
        else if (a == "--max-print") out.mismatch_print_limit = std::stoull(next("--max-print"));
        else if (a == "-h" || a == "--help") { print_usage(); std::exit(0); }
        else { std::cerr << "unknown arg: " << a << "\n"; return false; }
    }
    if (out.path.empty()) { std::cerr << "--path required\n"; return false; }
    return true;
}

std::shared_ptr<csdb::Database> open_db(const Args& args) {
#if defined(CSDB_USE_ROCKSDB)
    if (args.backend == "rocksdb") {
        auto db = std::make_shared<csdb::DatabaseRocksDB>();
        if (!db->open(args.path)) {
            std::cerr << "failed to open rocksdb at " << args.path << "\n";
            return nullptr;
        }
        return db;
    }
#endif
#if defined(CSDB_USE_BERKELEYDB)
    if (args.backend == "berkeleydb" || args.backend == "berkeley") {
        auto db = std::make_shared<csdb::DatabaseBerkeleyDB>();
        if (!db->open(args.path)) {
            std::cerr << "failed to open berkeleydb at " << args.path << "\n";
            return nullptr;
        }
        return db;
    }
#endif
    std::cerr << "backend not compiled in: " << args.backend << "\n";
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 2;
    }

    auto db = open_db(args);
    if (!db) return 2;

    cs::chain_integrity::Options opts;
    opts.progress_every = args.progress_every;
    opts.mismatch_print_limit = args.mismatch_print_limit;
    opts.progress_log = &std::cout;

    auto t0 = std::chrono::steady_clock::now();
    auto report = cs::chain_integrity::verify_range(*db, args.from, args.to, opts);
    auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "\n=== Summary ===\n"
              << "scanned         : " << report.scanned << " blocks (" << report.from << ".." << report.to << ")\n"
              << "elapsed         : " << totalSecs << "s\n"
              << "prev-hash/seq   : " << report.mismatches << " mismatches\n"
              << "gaps            : " << report.gaps << "\n"
              << "decode failures : " << report.decode_failures << "\n"
              << "missing pools   : " << report.missing << "\n";

    return report.ok() ? 0 : 1;
}
