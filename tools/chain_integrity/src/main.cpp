// chain_integrity: walk a CREDITS chain DB and verify hash chain + sequence monotonicity.
// Read-only; operates directly on the underlying database backend (no Storage::open).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <csdb/database.hpp>
#include <csdb/empty_pool_stub.hpp>
#include <csdb/pool.hpp>

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

    auto it = db->new_iterator();
    if (!it) {
        std::cerr << "iterator creation failed\n";
        return 2;
    }
    it->seek_to_last();
    cs::Sequence dbMax = 0;
    if (it->is_valid()) {
        dbMax = static_cast<cs::Sequence>(it->key());
        if (dbMax > 0) --dbMax;  // keys are seq+1
    }
    const cs::Sequence to = std::min(args.to, dbMax);

    std::cout << "chain_integrity: scanning " << args.from << ".." << to
              << " (db max seq = " << dbMax << ")\n";

    csdb::PoolHash prevHash;        // hash of (seq-1)
    bool havePrev = false;
    cs::Sequence prevSeq = 0;

    size_t mismatchCount = 0;
    size_t gapCount = 0;
    size_t decodeFailures = 0;
    size_t missingCount = 0;
    size_t printed = 0;

    auto t0 = std::chrono::steady_clock::now();

    for (cs::Sequence seq = args.from; seq <= to; ++seq) {
        cs::Bytes blob;
        const uint32_t key = static_cast<uint32_t>(seq + 1);   // csdb uses seq+1 as the key
        if (!db->get(key, &blob)) {
            ++missingCount;
            if (printed < args.mismatch_print_limit) {
                std::cout << "[MISSING] seq=" << seq << "\n";
                ++printed;
            }
            havePrev = false;
            continue;
        }
        csdb::Pool p = csdb::is_empty_pool_stub(blob)
            ? csdb::parse_empty_pool_stub(blob)
            : csdb::Pool::from_binary(std::move(blob));
        if (!p.is_valid()) {
            ++decodeFailures;
            if (printed < args.mismatch_print_limit) {
                std::cout << "[DECODE_FAIL] seq=" << seq << "\n";
                ++printed;
            }
            havePrev = false;
            continue;
        }
        if (p.sequence() != seq) {
            ++mismatchCount;
            if (printed < args.mismatch_print_limit) {
                std::cout << "[SEQ_MISMATCH] expected=" << seq
                          << " stored=" << p.sequence() << "\n";
                ++printed;
            }
        }
        if (havePrev) {
            if (seq != prevSeq + 1) {
                ++gapCount;
                if (printed < args.mismatch_print_limit) {
                    std::cout << "[GAP] expected_seq=" << (prevSeq + 1)
                              << " got=" << seq << "\n";
                    ++printed;
                }
            }
            if (!(p.previous_hash() == prevHash)) {
                ++mismatchCount;
                if (printed < args.mismatch_print_limit) {
                    std::cout << "[PREV_HASH] seq=" << seq
                              << " stored_prev=" << p.previous_hash().to_string()
                              << " expected_prev=" << prevHash.to_string()
                              << "\n";
                    ++printed;
                }
            }
        }

        prevHash = p.hash();
        prevSeq = seq;
        havePrev = true;

        if (args.progress_every > 0 && (seq - args.from) % args.progress_every == 0 && seq != args.from) {
            auto t = std::chrono::steady_clock::now();
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(t - t0).count();
            std::cout << "  ... seq=" << seq
                      << "  elapsed=" << secs << "s"
                      << "  mismatches=" << mismatchCount
                      << "  gaps=" << gapCount
                      << "  decode_fail=" << decodeFailures
                      << "  missing=" << missingCount
                      << "\n";
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count();
    std::cout << "\n=== Summary ===\n"
              << "scanned         : " << (to - args.from + 1) << " blocks (" << args.from << ".." << to << ")\n"
              << "elapsed         : " << totalSecs << "s\n"
              << "prev-hash/seq   : " << mismatchCount << " mismatches\n"
              << "gaps            : " << gapCount << "\n"
              << "decode failures : " << decodeFailures << "\n"
              << "missing pools   : " << missingCount << "\n";

    return (mismatchCount + gapCount + decodeFailures + missingCount) ? 1 : 0;
}
