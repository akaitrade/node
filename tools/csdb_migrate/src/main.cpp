// csdb_migrate: BerkeleyDB -> RocksDB one-shot migrator for CREDITS chain data.
// Reads source via DatabaseBerkeleyDB::new_iterator (no rescan / no signals) and
// rewrites every block via Storage::pool_save against a RocksDB destination.
// pool_save inside Storage applies the empty-block stub format and the async
// batched writer, so the destination ends up identical to a tinycs-rocksdb sync.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include <boost/filesystem.hpp>

#include <csdb/database_berkeleydb.hpp>
#include <csdb/database_rocksdb.hpp>
#include <csdb/empty_pool_stub.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage.hpp>

namespace fs = boost::filesystem;

namespace {

struct Args {
    std::string src;
    std::string dst;
    cs::Sequence from = 0;
    cs::Sequence to = std::numeric_limits<cs::Sequence>::max();
    size_t queue = 5000;
    size_t batch = 1000;
    size_t progress_every = 50000;
    bool force = false;
    bool verify = false;
    bool bulk_load = true;   // RocksDB bulk-load mode + final compact
    bool cross_verify = false;  // walk src+dst in lockstep, compare per-block fields
    bool validate_only = false; // skip migration; just cross-verify existing src vs dst
};

void print_usage() {
    std::cerr <<
        "csdb_migrate --src PATH --dst PATH [options]\n"
        "  --src PATH       Source BerkeleyDB directory\n"
        "  --dst PATH       Destination RocksDB directory (must be empty unless --force)\n"
        "  --from N         Start sequence (default 0)\n"
        "  --to   N         End sequence inclusive (default: end of source)\n"
        "  --queue N        Async write queue size (default 5000)\n"
        "  --batch N        Write batch size (default 1000)\n"
        "  --progress N     Progress log interval in blocks (default 50000)\n"
        "  --force          Allow non-empty destination directory\n"
        "  --verify         Spot-check counts after migration\n"
        "  --no-bulk-load   Disable RocksDB bulk-load mode (default: enabled)\n"
        "  --cross-verify   After migration, walk src+dst in lockstep and compare hash/prev_hash/confidants/user_fields/tx-count per block\n"
        "  --validate-only  Skip migration; just run --cross-verify against an existing dst (src and dst must both exist)\n";
}

bool parse_size(const char* s, size_t& out) {
    try { out = static_cast<size_t>(std::stoull(s)); return true; }
    catch (...) { return false; }
}

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::cerr << "missing value for " << name << "\n"; return nullptr; }
            return argv[++i];
        };
        if (k == "--src") { auto v = need_value("--src"); if (!v) return false; a.src = v; }
        else if (k == "--dst") { auto v = need_value("--dst"); if (!v) return false; a.dst = v; }
        else if (k == "--from") { auto v = need_value("--from"); if (!v) return false; size_t n; if (!parse_size(v, n)) return false; a.from = static_cast<cs::Sequence>(n); }
        else if (k == "--to") { auto v = need_value("--to"); if (!v) return false; size_t n; if (!parse_size(v, n)) return false; a.to = static_cast<cs::Sequence>(n); }
        else if (k == "--queue") { auto v = need_value("--queue"); if (!v) return false; if (!parse_size(v, a.queue)) return false; }
        else if (k == "--batch") { auto v = need_value("--batch"); if (!v) return false; if (!parse_size(v, a.batch)) return false; }
        else if (k == "--progress") { auto v = need_value("--progress"); if (!v) return false; if (!parse_size(v, a.progress_every)) return false; }
        else if (k == "--force") { a.force = true; }
        else if (k == "--verify") { a.verify = true; }
        else if (k == "--no-bulk-load") { a.bulk_load = false; }
        else if (k == "--cross-verify") { a.cross_verify = true; }
        else if (k == "--validate-only") { a.validate_only = true; a.cross_verify = true; }
        else if (k == "--help" || k == "-h") { print_usage(); return false; }
        else { std::cerr << "unknown argument: " << k << "\n"; print_usage(); return false; }
    }
    if (a.src.empty() || a.dst.empty()) { print_usage(); return false; }
    return true;
}

bool dst_dir_empty(const std::string& path) {
    fs::path p(path);
    if (!fs::exists(p)) return true;
    if (!fs::is_directory(p)) return false;
    return fs::directory_iterator(p) == fs::directory_iterator();
}

std::string format_duration(std::chrono::milliseconds ms) {
    using namespace std::chrono;
    auto h = duration_cast<hours>(ms); ms -= h;
    auto m = duration_cast<minutes>(ms); ms -= m;
    auto s = duration_cast<seconds>(ms);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02lldh%02lldm%02llds",
                  static_cast<long long>(h.count()),
                  static_cast<long long>(m.count()),
                  static_cast<long long>(s.count()));
    return buf;
}

// Walk src + dst in lockstep by sequence and compare integrity-critical fields.
// For tx-blocks dst stores the full Pool::to_binary() and we additionally
// require byte-for-byte equality. For empty blocks dst stores a stub; we
// reconstruct via parse_empty_pool_stub and compare the lossless fields.
bool validate_chain(csdb::Database& src, csdb::Database& dst, const Args& args) {
    auto src_it = src.new_iterator();
    auto dst_it = dst.new_iterator();
    if (!src_it || !dst_it) {
        std::cerr << "validate: iterator unavailable\n";
        return false;
    }

    src_it->seek_to_first();
    dst_it->seek_to_first();

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    size_t checked = 0;
    size_t stub_count = 0;
    size_t full_count = 0;

    while (src_it->is_valid() && dst_it->is_valid()) {
        const uint32_t src_seq = src_it->key();
        const uint32_t dst_seq = dst_it->key();
        if (src_seq != dst_seq) {
            std::cerr << "validate: seq mismatch  src=" << src_seq
                      << "  dst=" << dst_seq << "\n";
            return false;
        }
        if (static_cast<cs::Sequence>(src_seq) > args.to) break;
        if (static_cast<cs::Sequence>(src_seq) < args.from) {
            src_it->next();
            dst_it->next();
            continue;
        }

        cs::Bytes src_raw = src_it->value();
        cs::Bytes dst_raw = dst_it->value();
        const bool dst_is_stub = csdb::is_empty_pool_stub(dst_raw);

        cs::Bytes src_raw_copy = src_raw;   // from_binary moves; keep a copy for byte-compare
        csdb::Pool src_pool = csdb::Pool::from_binary(std::move(src_raw_copy));
        csdb::Pool dst_pool = dst_is_stub
            ? csdb::parse_empty_pool_stub(dst_raw)
            : csdb::Pool::from_binary(cs::Bytes(dst_raw));

        if (!src_pool.is_valid() || !dst_pool.is_valid()) {
            std::cerr << "validate: parse failed at seq=" << src_seq
                      << "  src_valid=" << src_pool.is_valid()
                      << "  dst_valid=" << dst_pool.is_valid() << "\n";
            return false;
        }

        if (src_pool.sequence() != dst_pool.sequence()) {
            std::cerr << "validate: sequence mismatch at " << src_seq
                      << "  src.seq=" << src_pool.sequence()
                      << "  dst.seq=" << dst_pool.sequence() << "\n";
            return false;
        }
        if (src_pool.hash() != dst_pool.hash()) {
            std::cerr << "validate: hash mismatch at seq=" << src_seq << "\n"
                      << "  src: " << src_pool.hash().to_string() << "\n"
                      << "  dst: " << dst_pool.hash().to_string() << "\n";
            return false;
        }
        if (src_pool.previous_hash() != dst_pool.previous_hash()) {
            std::cerr << "validate: previous_hash mismatch at seq=" << src_seq << "\n"
                      << "  src: " << src_pool.previous_hash().to_string() << "\n"
                      << "  dst: " << dst_pool.previous_hash().to_string() << "\n";
            return false;
        }
        if (src_pool.confidants() != dst_pool.confidants()) {
            std::cerr << "validate: confidants mismatch at seq=" << src_seq
                      << "  src.n=" << src_pool.confidants().size()
                      << "  dst.n=" << dst_pool.confidants().size() << "\n";
            return false;
        }
        if (src_pool.serialize_user_fields() != dst_pool.serialize_user_fields()) {
            std::cerr << "validate: user_fields mismatch at seq=" << src_seq << "\n";
            return false;
        }
        if (src_pool.transactions().size() != dst_pool.transactions().size()) {
            std::cerr << "validate: tx count mismatch at seq=" << src_seq
                      << "  src=" << src_pool.transactions().size()
                      << "  dst=" << dst_pool.transactions().size() << "\n";
            return false;
        }
        // For non-stub dst encoding, raw bytes must match the source exactly.
        if (!dst_is_stub && src_raw != dst_raw) {
            std::cerr << "validate: raw binary mismatch at seq=" << src_seq
                      << "  (full-encoded block; expected byte-for-byte equality)\n";
            return false;
        }

        if (dst_is_stub) ++stub_count; else ++full_count;
        ++checked;

        src_it->next();
        dst_it->next();

        if (checked % args.progress_every == 0) {
            const auto now = clock::now();
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0);
            const double rate = since_last > 0
                ? (1000.0 * static_cast<double>(args.progress_every) / static_cast<double>(since_last))
                : 0.0;
            std::cout << "  validated=" << checked
                      << "  seq=" << src_seq
                      << "  stubs=" << stub_count
                      << "  full=" << full_count
                      << "  rate=" << static_cast<uint64_t>(rate) << "/s"
                      << "  elapsed=" << format_duration(elapsed)
                      << std::endl;
            last_log = now;
        }
    }

    // If one side ran out before --to, the chains have different lengths.
    if (src_it->is_valid() && static_cast<cs::Sequence>(src_it->key()) <= args.to) {
        std::cerr << "validate: src has more blocks than dst (next src seq=" << src_it->key() << ")\n";
        return false;
    }
    if (dst_it->is_valid() && static_cast<cs::Sequence>(dst_it->key()) <= args.to) {
        std::cerr << "validate: dst has more blocks than src (next dst seq=" << dst_it->key() << ")\n";
        return false;
    }

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0);
    std::cout << "cross-verify OK"
              << "  validated=" << checked
              << "  stubs=" << stub_count
              << "  full=" << full_count
              << "  elapsed=" << format_duration(total)
              << std::endl;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    if (!fs::exists(args.src)) {
        std::cerr << "src does not exist: " << args.src << "\n";
        return 2;
    }
    if (args.validate_only) {
        if (!fs::exists(args.dst) || dst_dir_empty(args.dst)) {
            std::cerr << "validate-only: dst does not exist or is empty: " << args.dst << "\n";
            return 2;
        }
        std::cout << "csdb_migrate: validate-only"
                  << "\n  src: " << args.src
                  << "\n  dst: " << args.dst
                  << "\n  from: " << args.from
                  << "\n  to: " << (args.to == std::numeric_limits<cs::Sequence>::max() ? std::string("end") : std::to_string(args.to))
                  << std::endl;

        csdb::DatabaseBerkeleyDB v_src;
        if (!v_src.open(args.src)) {
            std::cerr << "failed to open BerkeleyDB at " << args.src
                      << ": " << v_src.last_error_message() << "\n";
            return 1;
        }
        auto v_dst = std::make_shared<csdb::DatabaseRocksDB>();
        if (!v_dst->open(args.dst)) {
            std::cerr << "failed to open RocksDB at " << args.dst
                      << ": " << v_dst->last_error_message() << "\n";
            return 1;
        }
        csdb::Database& v_src_base = v_src;
        csdb::Database& v_dst_base = *v_dst;
        return validate_chain(v_src_base, v_dst_base, args) ? 0 : 1;
    }
    if (!args.force && !dst_dir_empty(args.dst)) {
        std::cerr << "dst is not empty: " << args.dst
                  << "\n  Pass --force to override (note: chain check requires a fresh dst).\n";
        return 2;
    }
    fs::create_directories(args.dst);

    csdb::DatabaseBerkeleyDB src_db;
    if (!src_db.open(args.src)) {
        std::cerr << "failed to open BerkeleyDB at " << args.src
                  << ": " << src_db.last_error_message() << "\n";
        return 1;
    }

    auto dst_db = std::make_shared<csdb::DatabaseRocksDB>();
    if (args.bulk_load) {
        dst_db->set_bulk_load(true);
    }
    if (!dst_db->open(args.dst)) {
        std::cerr << "failed to open RocksDB at " << args.dst
                  << ": " << dst_db->last_error_message() << "\n";
        return 1;
    }

    csdb::Storage::OpenOptions opt;
    opt.db = dst_db;
    opt.newBlockchainTop = cs::kWrongSequence;
    opt.startSequence = 0;
    opt.asyncWriteQueueMax = args.queue;
    opt.writeBatchSize = args.batch;

    csdb::Storage dst;
    if (!dst.open(opt)) {
        std::cerr << "failed to open destination Storage: "
                  << dst.last_error_message() << "\n";
        return 1;
    }

    std::cout << "csdb_migrate"
              << "\n  src: "      << args.src
              << "\n  dst: "      << args.dst
              << "\n  from: "     << args.from
              << "\n  to: "       << (args.to == std::numeric_limits<cs::Sequence>::max() ? std::string("end") : std::to_string(args.to))
              << "\n  queue: "    << args.queue
              << "\n  batch: "    << args.batch
              << "\n  progress: " << args.progress_every
              << std::endl;

    csdb::Database& src_base = src_db;          // new_iterator is public on Database
    auto it = src_base.new_iterator();
    if (!it) {
        std::cerr << "src iterator unavailable\n";
        return 1;
    }

    if (args.from > 0) {
        it->seek(static_cast<uint32_t>(args.from));
    } else {
        it->seek_to_first();
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    size_t saved = 0;
    uint64_t empty_blocks = 0;
    uint64_t tx_blocks = 0;

    // Per-phase timing in nanoseconds, accumulated across the progress window
    // and reset at each progress tick. read = BDB cursor advance + value fetch,
    // parse = Pool::from_binary, save = Storage::pool_save (queue push +
    // bookkeeping). Writer thread runs in parallel, so save here mostly
    // measures the queue-push critical section.
    int64_t ns_read = 0, ns_parse = 0, ns_save = 0;

    while (it->is_valid()) {
        const auto ts_a = clock::now();
        const uint32_t seq_key = it->key();
        if (static_cast<cs::Sequence>(seq_key) > args.to) break;
        cs::Bytes raw = it->value();
        const auto ts_b = clock::now();

        csdb::Pool pool = csdb::Pool::from_binary(std::move(raw));
        const auto ts_c = clock::now();

        if (!pool.is_valid()) {
            std::cerr << "src seq=" << seq_key << " not parseable; aborting\n";
            return 1;
        }
        if (pool.sequence() != static_cast<cs::Sequence>(seq_key)) {
            std::cerr << "src seq mismatch: key=" << seq_key
                      << " pool.sequence=" << pool.sequence() << "\n";
            return 1;
        }
        if (pool.transactions().empty()) ++empty_blocks; else ++tx_blocks;

        if (!dst.pool_save(pool)) {
            std::cerr << "dst pool_save failed at seq=" << seq_key << ": "
                      << dst.last_error_message()
                      << " (db: " << dst.db_last_error_message() << ")\n";
            return 1;
        }
        const auto ts_d = clock::now();
        ++saved;

        // Advance the cursor (BDB disk read happens here for next iter).
        const auto ts_e = clock::now();
        it->next();
        const auto ts_f = clock::now();

        ns_read  += std::chrono::duration_cast<std::chrono::nanoseconds>(ts_b - ts_a).count();
        ns_read  += std::chrono::duration_cast<std::chrono::nanoseconds>(ts_f - ts_e).count();
        ns_parse += std::chrono::duration_cast<std::chrono::nanoseconds>(ts_c - ts_b).count();
        ns_save  += std::chrono::duration_cast<std::chrono::nanoseconds>(ts_d - ts_c).count();

        if (saved % args.progress_every == 0) {
            const auto now = clock::now();
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0);
            const double recent_rate =
                since_last > 0 ? (1000.0 * static_cast<double>(args.progress_every) / static_cast<double>(since_last)) : 0.0;
            const double n = static_cast<double>(args.progress_every);
            std::cout << "  seq=" << seq_key
                      << "  saved=" << saved
                      << "  empty=" << empty_blocks
                      << "  tx=" << tx_blocks
                      << "  rate=" << static_cast<uint64_t>(recent_rate) << "/s"
                      << "  read=" << static_cast<uint64_t>(ns_read / n / 1000.0) << "us"
                      << "  parse=" << static_cast<uint64_t>(ns_parse / n / 1000.0) << "us"
                      << "  save=" << static_cast<uint64_t>(ns_save / n / 1000.0) << "us"
                      << "  elapsed=" << format_duration(elapsed)
                      << std::endl;
            last_log = now;
            ns_read = ns_parse = ns_save = 0;
        }
    }

    std::cout << "draining writer ..." << std::endl;
    dst.close();

    if (args.bulk_load) {
        std::cout << "compacting destination (bulk-load mode) ..." << std::endl;
        const auto t_compact = clock::now();
        if (!dst_db->compact_full()) {
            std::cerr << "compact_full failed: " << dst_db->last_error_message() << "\n";
            return 1;
        }
        const auto compact_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_compact);
        std::cout << "compact done in " << format_duration(compact_ms) << std::endl;
    }

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0);
    std::cout << "done"
              << "  saved=" << saved
              << "  empty=" << empty_blocks
              << "  tx=" << tx_blocks
              << "  elapsed=" << format_duration(total)
              << std::endl;

    if (args.verify) {
        std::cout << "verify: scanning destination ..." << std::endl;
        // Storage::close() dropped its shared_ptr but the migrate tool still
        // holds dst_db, so the rocksdb is open and the LOCK file is ours.
        // Iterate that handle directly instead of re-opening (which would
        // deadlock on the LOCK).
        csdb::Database& v_base = *dst_db;
        auto vit = v_base.new_iterator();
        if (!vit) { std::cerr << "verify: iterator unavailable\n"; return 1; }
        size_t count = 0;
        cs::Sequence last_seq = std::numeric_limits<cs::Sequence>::max();
        for (vit->seek_to_first(); vit->is_valid(); vit->next()) {
            ++count;
            last_seq = static_cast<cs::Sequence>(vit->key());
        }
        std::cout << "verify: dst count=" << count << "  last_seq=" << last_seq << std::endl;
        if (count != saved) {
            std::cerr << "verify: count mismatch (saved=" << saved
                      << "  dst=" << count << ")\n";
            return 1;
        }
        std::cout << "verify: OK" << std::endl;
    }

    if (args.cross_verify) {
        std::cout << "cross-verify: comparing src vs dst per-block ..." << std::endl;
        csdb::Database& cv_src = src_db;
        csdb::Database& cv_dst = *dst_db;
        if (!validate_chain(cv_src, cv_dst, args)) {
            return 1;
        }
    }

    return 0;
}
