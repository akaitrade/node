#include <csnode/chain_integrity.hpp>

#include <chrono>
#include <ostream>

#include <csdb/database.hpp>
#include <csdb/empty_pool_stub.hpp>
#include <csdb/pool.hpp>

#if defined(CSDB_USE_ROCKSDB)
#include <csdb/database_rocksdb.hpp>
#endif
#if defined(CSDB_USE_BERKELEYDB)
#include <csdb/database_berkeleydb.hpp>
#endif

namespace cs::chain_integrity {

cs::Sequence db_top_sequence(csdb::Database& db) {
    auto it = db.new_iterator();
    if (!it) return 0;
    it->seek_to_last();
    if (!it->is_valid()) return 0;
    cs::Sequence key = static_cast<cs::Sequence>(it->key());
    return key > 0 ? key - 1 : 0;
}

bool verify_at(csdb::Database& db, cs::Sequence seq, const cs::Bytes& expected_hash) {
    cs::Bytes blob;
    // Database::get(uint32_t) internally maps seq -> key (seq+1). Pass the
    // sequence as-is, NOT seq+1 — otherwise we read the next block.
    if (!db.get(static_cast<uint32_t>(seq), &blob)) return false;
    csdb::Pool p = csdb::is_empty_pool_stub(blob)
        ? csdb::parse_empty_pool_stub(blob)
        : csdb::Pool::from_binary(std::move(blob));
    if (!p.is_valid()) return false;
    if (p.sequence() != seq) return false;
    if (!expected_hash.empty() && p.hash().to_binary() != expected_hash) return false;
    return true;
}

Report verify_range(csdb::Database& db, cs::Sequence from, cs::Sequence to, const Options& opts) {
    Report rep;
    rep.from = from;

    const cs::Sequence dbMax = db_top_sequence(db);
    const cs::Sequence end = std::min(to, dbMax);
    rep.to = end;

    if (opts.progress_log) {
        *opts.progress_log << "chain_integrity: scanning " << from << ".." << end
                           << " (db max seq = " << dbMax << ")\n";
    }

    csdb::PoolHash prevHash;
    bool havePrev = false;
    cs::Sequence prevSeq = 0;
    size_t printed = 0;

    auto t0 = std::chrono::steady_clock::now();

    for (cs::Sequence seq = from; seq <= end; ++seq) {
        cs::Bytes blob;
        // see note in verify_at: Database::get(uint32_t) already does seq->key remap
        if (!db.get(static_cast<uint32_t>(seq), &blob)) {
            ++rep.missing;
            if (opts.progress_log && printed < opts.mismatch_print_limit) {
                *opts.progress_log << "[MISSING] seq=" << seq << "\n";
                ++printed;
            }
            havePrev = false;
            continue;
        }
        csdb::Pool p = csdb::is_empty_pool_stub(blob)
            ? csdb::parse_empty_pool_stub(blob)
            : csdb::Pool::from_binary(std::move(blob));
        if (!p.is_valid()) {
            ++rep.decode_failures;
            if (opts.progress_log && printed < opts.mismatch_print_limit) {
                *opts.progress_log << "[DECODE_FAIL] seq=" << seq << "\n";
                ++printed;
            }
            havePrev = false;
            continue;
        }
        if (p.sequence() != seq) {
            ++rep.mismatches;
            if (opts.progress_log && printed < opts.mismatch_print_limit) {
                *opts.progress_log << "[SEQ_MISMATCH] expected=" << seq
                                   << " stored=" << p.sequence() << "\n";
                ++printed;
            }
        }
        if (havePrev) {
            if (seq != prevSeq + 1) {
                ++rep.gaps;
                if (opts.progress_log && printed < opts.mismatch_print_limit) {
                    *opts.progress_log << "[GAP] expected_seq=" << (prevSeq + 1)
                                       << " got=" << seq << "\n";
                    ++printed;
                }
            }
            if (!(p.previous_hash() == prevHash)) {
                ++rep.mismatches;
                if (opts.progress_log && printed < opts.mismatch_print_limit) {
                    *opts.progress_log << "[PREV_HASH] seq=" << seq
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
        ++rep.scanned;

        if (opts.progress_log && opts.progress_every > 0
                && (seq - from) % opts.progress_every == 0 && seq != from) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0).count();
            *opts.progress_log << "  ... seq=" << seq
                               << "  elapsed=" << secs << "s"
                               << "  mismatches=" << rep.mismatches
                               << "  gaps=" << rep.gaps
                               << "  decode_fail=" << rep.decode_failures
                               << "  missing=" << rep.missing
                               << "\n";
        }
    }

    return rep;
}

std::shared_ptr<csdb::Database> open_db(const std::string& backend, const std::string& path) {
#if defined(CSDB_USE_ROCKSDB)
    if (backend == "rocksdb") {
        auto db = std::make_shared<csdb::DatabaseRocksDB>();
        if (!db->open(path)) return nullptr;
        return db;
    }
#endif
#if defined(CSDB_USE_BERKELEYDB)
    if (backend == "berkeleydb" || backend == "berkeley") {
        auto db = std::make_shared<csdb::DatabaseBerkeleyDB>();
        if (!db->open(path)) return nullptr;
        return db;
    }
#endif
    // "both" or unknown: prefer the backend compiled in as default.
#if defined(CSDB_USE_BERKELEYDB)
    if (backend.empty() || backend == "both") {
        auto db = std::make_shared<csdb::DatabaseBerkeleyDB>();
        if (db->open(path)) return db;
    }
#endif
#if defined(CSDB_USE_ROCKSDB)
    if (backend.empty() || backend == "both") {
        auto db = std::make_shared<csdb::DatabaseRocksDB>();
        if (db->open(path)) return db;
    }
#endif
    return nullptr;
}

} // namespace cs::chain_integrity
