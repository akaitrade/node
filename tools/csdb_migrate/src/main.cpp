// csdb_migrate: BerkeleyDB -> RocksDB one-shot migrator for CREDITS chain data.
// Reads source via DatabaseBerkeleyDB::new_iterator (no rescan / no signals) and
// rewrites every block via Storage::pool_save against a RocksDB destination.
// pool_save inside Storage applies the empty-block stub format and the async
// batched writer, so the destination ends up identical to a tinycs-rocksdb sync.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include <lz4.h>

#include <csdb/amount_commission.hpp>
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
    bool use_stubs = true;      // write empty pools as compact stubs in dst
    bool verify_balances = false; // replay both DBs through simplified balance accounting
    size_t mismatch_print_limit = 20; // first N mismatching wallets printed in detail
    std::string dst_backend = "rocksdb"; // "rocksdb" or "berkeley"
    bool confidant_indirection = false;  // experimental: dedup confidants into a side table
    std::string confidant_sets_out;      // side-file path for the set table (default: <dst>/confidant_sets.bin)
    size_t bundle_size = 0;              // experimental: pack N blocks per BDB record (0 = disabled)
    bool bundle_compress = false;        // LZ4-compress each bundle value before put_recno
    bool pubkey_remap = false;           // experimental: rewrite tx src/tgt + confidants as wallet_id refs
};

// Reserved user_field id for the experimental confidant-set reference. Out of
// the existing range (kFieldTimestamp=0, kFieldServiceInfo=1, kFieldBlockReward=3).
constexpr csdb::user_field_id_t kFieldConfidantSetIdExp = 99;

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
        "  --validate-only  Skip migration; just run --cross-verify against an existing dst (src and dst must both exist)\n"
        "  --no-stubs       Write empty pools as full Pool::to_binary() instead of compact stubs (default: stubs enabled)\n"
        "  --dst-backend B  Destination backend: 'rocksdb' (default) or 'berkeley'. Use 'berkeley' to apply v4 stubs in-place to a BerkeleyDB chain without changing storage engine.\n"
        "  --verify-balances Replay src and dst through simplified native-coin accounting and compare end-state wallet maps. Catches lossy on-disk format bugs that affect balances (e.g. dropped realTrusted_ in stubs). Skips smart-contract execution and delegation.\n"
        "  --confidant-indirection  Experimental: replace each block's confidants vector with a 4-byte set id (user_field 99) and emit a side file 'confidant_sets.bin' with the dedup table. Use --dst-backend berkeley. The dst chain is for size measurement only and is NOT node-loadable.\n"
        "  --confidant-sets-out F   Override the side-file path (default: <dst>/confidant_sets.bin)\n"
        "  --bundle-size N          Experimental: pack N consecutive blocks into one BDB record (key = first_seq/N + 1). Requires --dst-backend berkeley. Skips Storage. dst is NOT node-loadable.\n"
        "  --bundle-compress        LZ4-compress each bundle value before write (use with --bundle-size).\n"
        "  --pubkey-remap           Experimental: rewrite every transaction's source/target as wallet-id Address (4B) and pack confidants into user_field 99 as a [count:2][ids:n*4] blob. Pass 1 scans src to build the dict; pass 2 rewrites and writes. Emits 'pubkey_dict.bin'. Combine with --bundle-size and/or --bundle-compress. dst is NOT node-loadable.\n";
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
        else if (k == "--no-stubs") { a.use_stubs = false; }
        else if (k == "--dst-backend") {
            auto v = need_value("--dst-backend"); if (!v) return false;
            a.dst_backend = v;
            if (a.dst_backend != "rocksdb" && a.dst_backend != "berkeley") {
                std::cerr << "--dst-backend must be 'rocksdb' or 'berkeley'\n";
                return false;
            }
        }
        else if (k == "--verify-balances") { a.verify_balances = true; }
        else if (k == "--confidant-indirection") { a.confidant_indirection = true; }
        else if (k == "--confidant-sets-out") { auto v = need_value("--confidant-sets-out"); if (!v) return false; a.confidant_sets_out = v; }
        else if (k == "--bundle-size") { auto v = need_value("--bundle-size"); if (!v) return false; if (!parse_size(v, a.bundle_size)) return false; }
        else if (k == "--bundle-compress") { a.bundle_compress = true; }
        else if (k == "--pubkey-remap") { a.pubkey_remap = true; }
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
        // numberTrusted_ and realTrusted_ feed bitsToMask() in reward
        // distribution. v3 stubs lost both → block reward undercounting.
        // v4 stubs preserve realTrusted; numberTrusted derives from confCount.
        // Cross-verify catches any future format that silently drops them.
        if (src_pool.numberTrusted() != dst_pool.numberTrusted()) {
            std::cerr << "validate: numberTrusted mismatch at seq=" << src_seq
                      << "  src=" << static_cast<int>(src_pool.numberTrusted())
                      << "  dst=" << static_cast<int>(dst_pool.numberTrusted()) << "\n";
            return false;
        }
        if (src_pool.realTrusted() != dst_pool.realTrusted()) {
            std::cerr << "validate: realTrusted mismatch at seq=" << src_seq
                      << "  src=" << src_pool.realTrusted()
                      << "  dst=" << dst_pool.realTrusted() << "\n";
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

// ----------------------------------------------------------------------------
// --verify-balances: simplified native-coin balance reconstruction.
//
// What this catches: any difference between src and dst that affects native
// coin balances. The realTrusted_-loss bug in v3 stubs is the canonical case:
// dst's parsed Pool has realTrusted=0 → no fee/reward distribution → confidant
// addresses end up with lower balances than on src.
//
// What this does NOT model: smart contract execution (new_state, emitted txns,
// payable cost), delegation (sources/targets), inner-tx flows. The same
// simplification applies to both src and dst, so identical input data yields
// identical outputs — divergence is purely from on-disk format differences.
//
// Inspection cost: O(blocks) per side; ~5-15 min on a 173M-block chain.
// ----------------------------------------------------------------------------

constexpr csdb::user_field_id_t kFieldBlockReward = 3;   // mirrors BlockChain::kFieldBlockReward
constexpr uint8_t kUntrustedMarker = 255;                // mirrors cs::kUntrustedMarker

struct WalletState {
    csdb::Amount balance{0};
    uint64_t txCount = 0;
};

using WalletMap = std::map<cs::PublicKey, WalletState>;

// Mirrors cs::Utils::bitsToMask(numberTrusted, realTrusted): returns a vector
// of size numberTrusted whose entries are kUntrustedMarker for non-participants
// and the trusted-position index (incrementing 0,1,2,...) for participants.
std::vector<uint8_t> bits_to_mask(uint8_t numberTrusted, uint64_t realTrusted) {
    std::vector<uint8_t> mask(numberTrusted, kUntrustedMarker);
    uint8_t pos = 0;
    for (uint8_t i = 0; i < numberTrusted; ++i) {
        if (realTrusted & (1ULL << i)) {
            mask[i] = pos++;
        }
    }
    return mask;
}

std::string hex_pubkey(const cs::PublicKey& key) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto b : key) {
        out.push_back(digits[(b >> 4) & 0xF]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

WalletMap compute_balances(csdb::Database& db,
                           cs::Sequence from, cs::Sequence to,
                           const std::string& tag) {
    WalletMap wallets;
    auto it = db.new_iterator();
    if (!it) {
        std::cerr << "verify-balances[" << tag << "]: iterator unavailable\n";
        return wallets;
    }

    if (from > 0) it->seek(static_cast<uint32_t>(from));
    else it->seek_to_first();

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    size_t blocks = 0;
    size_t skipped = 0;
    size_t with_rewards = 0;

    while (it->is_valid()) {
        uint32_t seq = it->key();
        if (static_cast<cs::Sequence>(seq) > to) break;

        cs::Bytes raw = it->value();
        csdb::Pool pool = csdb::is_empty_pool_stub(raw)
            ? csdb::parse_empty_pool_stub(raw)
            : csdb::Pool::from_binary(std::move(raw));

        if (!pool.is_valid()) {
            ++skipped;
            it->next();
            continue;
        }

        // 1) Per-transaction native-coin accounting.
        csdb::Amount totalCountedFee{0};
        for (const auto& tr : pool.transactions()) {
            cs::PublicKey srcKey{}, dstKey{};
            const auto& srcPk = tr.source().public_key();
            const auto& dstPk = tr.target().public_key();
            if (srcPk.size() == srcKey.size())
                std::memcpy(srcKey.data(), srcPk.data(), srcKey.size());
            if (dstPk.size() == dstKey.size())
                std::memcpy(dstKey.data(), dstPk.data(), dstKey.size());

            csdb::Amount amount = tr.amount();
            csdb::Amount maxFee(tr.max_fee().to_double());
            csdb::Amount countedFee(tr.counted_fee().to_double());

            // Source: held max_fee + amount; refund the unused portion of max_fee.
            wallets[srcKey].balance -= (amount + maxFee);
            wallets[srcKey].balance += (maxFee - countedFee);
            wallets[srcKey].txCount++;

            wallets[dstKey].balance += amount;
            wallets[dstKey].txCount++;

            totalCountedFee += countedFee;
        }

        // 2) Fee + block-reward distribution to confidants based on realTrusted.
        const auto& confidants = pool.confidants();
        const uint8_t  nTrust       = pool.numberTrusted();
        const uint64_t realTrusted  = pool.realTrusted();

        if (nTrust > 0 && !confidants.empty() && realTrusted != 0) {
            auto mask = bits_to_mask(nTrust, realTrusted);

            int realCount = 0;
            for (auto m : mask) if (m != kUntrustedMarker) ++realCount;

            // 2a) Fees: split totalCountedFee evenly across real-trusted confidants.
            if (realCount > 0 && totalCountedFee > csdb::Amount{0}) {
                csdb::Amount perFee = totalCountedFee / realCount;
                csdb::Amount distributed{0};
                int paid = 0;
                for (size_t i = 0; i < confidants.size() && i < mask.size(); ++i) {
                    if (mask[i] == kUntrustedMarker) continue;
                    csdb::Amount fee = (paid == realCount - 1)
                        ? (totalCountedFee - distributed) : perFee;
                    wallets[confidants[i]].balance += fee;
                    distributed += fee;
                    ++paid;
                }
            }

            // 2b) Block reward: read kFieldBlockReward (mining era), one
            // (int32 LE, uint64 LE) pair per mask entry.
            auto fld = pool.user_field(kFieldBlockReward);
            if (fld.is_valid()) {
                std::string data = fld.value<std::string>();
                size_t off = 0;
                std::vector<csdb::Amount> rewards;
                rewards.reserve(mask.size());
                for (size_t k = 0; k < mask.size(); ++k) {
                    if (off + 12 > data.size()) break;
                    int32_t  integer  = 0;
                    uint64_t fraction = 0;
                    std::memcpy(&integer,  data.data() + off, 4); off += 4;
                    std::memcpy(&fraction, data.data() + off, 8); off += 8;
                    rewards.emplace_back(integer, fraction);
                }

                if (rewards.size() == mask.size()) {
                    ++with_rewards;
                    for (size_t i = 0; i < confidants.size() && i < mask.size(); ++i) {
                        if (mask[i] == kUntrustedMarker) continue;
                        csdb::Amount reward = rewards[i];
                        if (reward < csdb::Amount{0}) reward = csdb::Amount{0};
                        wallets[confidants[i]].balance += reward;
                    }
                }
            }
        }

        ++blocks;
        it->next();

        if (blocks % 100000 == 0) {
            const auto now = clock::now();
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0);
            const double rate = since_last > 0
                ? (1000.0 * 100000.0 / static_cast<double>(since_last))
                : 0.0;
            std::cout << "  [" << tag << "] blocks=" << blocks
                      << "  seq=" << seq
                      << "  wallets=" << wallets.size()
                      << "  rate=" << static_cast<uint64_t>(rate) << "/s"
                      << "  elapsed=" << format_duration(elapsed)
                      << std::endl;
            last_log = now;
        }
    }

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0);
    std::cout << "verify-balances[" << tag << "]: blocks=" << blocks
              << "  wallets=" << wallets.size()
              << "  reward_blocks=" << with_rewards
              << "  skipped=" << skipped
              << "  elapsed=" << format_duration(total)
              << std::endl;
    return wallets;
}

bool verify_balances(csdb::Database& src, csdb::Database& dst, const Args& args) {
    std::cout << "verify-balances: replaying src..." << std::endl;
    auto srcMap = compute_balances(src, args.from, args.to, "src");

    std::cout << "verify-balances: replaying dst..." << std::endl;
    auto dstMap = compute_balances(dst, args.from, args.to, "dst");

    size_t mismatches = 0;
    size_t srcOnly = 0;
    size_t dstOnly = 0;

    for (const auto& [key, srcState] : srcMap) {
        auto it = dstMap.find(key);
        if (it == dstMap.end()) {
            ++srcOnly;
            if (srcOnly <= args.mismatch_print_limit) {
                std::cerr << "  SRC-ONLY  " << hex_pubkey(key)
                          << "  bal=" << srcState.balance.to_string() << "\n";
            }
            continue;
        }
        if (srcState.balance != it->second.balance) {
            if (mismatches < args.mismatch_print_limit) {
                std::cerr << "  MISMATCH  " << hex_pubkey(key)
                          << "  src=" << srcState.balance.to_string()
                          << "  dst=" << it->second.balance.to_string()
                          << "  diff=" << (srcState.balance - it->second.balance).to_string()
                          << "\n";
            }
            ++mismatches;
        }
    }
    for (const auto& [key, dstState] : dstMap) {
        if (srcMap.find(key) == srcMap.end()) {
            ++dstOnly;
            if (dstOnly <= args.mismatch_print_limit) {
                std::cerr << "  DST-ONLY  " << hex_pubkey(key)
                          << "  bal=" << dstState.balance.to_string() << "\n";
            }
        }
    }

    std::cout << "verify-balances: mismatches=" << mismatches
              << "  src_only=" << srcOnly
              << "  dst_only=" << dstOnly
              << "  src_wallets=" << srcMap.size()
              << "  dst_wallets=" << dstMap.size()
              << std::endl;

    return mismatches == 0 && srcOnly == 0 && dstOnly == 0;
}

// Confidant-set deduplication. Key = byte-concat of all 32B pubkeys in order.
// Sets are assigned IDs 0..N-1 in first-seen order. Reused across blocks.
struct ConfidantInterner {
    std::map<std::string, uint32_t> dedup;
    std::vector<std::vector<cs::PublicKey>> sets;
    uint64_t blocks_with_conf = 0;
    uint64_t blocks_empty_conf = 0;
    uint64_t inline_bytes_today = 0;  // sum of n*32 across all blocks (what we remove)

    uint32_t intern(const std::vector<cs::PublicKey>& pks) {
        if (pks.empty()) { ++blocks_empty_conf; return 0; }
        ++blocks_with_conf;
        inline_bytes_today += pks.size() * 32;
        std::string key;
        key.reserve(pks.size() * 32);
        for (const auto& pk : pks) {
            key.append(reinterpret_cast<const char*>(pk.data()), pk.size());
        }
        auto it = dedup.find(key);
        if (it != dedup.end()) return it->second;
        const uint32_t id = static_cast<uint32_t>(sets.size());
        dedup.emplace(std::move(key), id);
        sets.push_back(pks);
        return id;
    }

    uint64_t table_bytes() const {
        // Side-file layout: [magic:4='CSET'][version:1=1][set_count:4 LE]
        //   per set: [n_conf:1][conf:n*32]
        uint64_t bytes = 4 + 1 + 4;
        for (const auto& s : sets) bytes += 1 + s.size() * 32;
        return bytes;
    }

    bool dump(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const char magic[4] = {'C', 'S', 'E', 'T'};
        const uint8_t version = 1;
        const uint32_t count = static_cast<uint32_t>(sets.size());
        std::fwrite(magic, 1, 4, f);
        std::fwrite(&version, 1, 1, f);
        std::fwrite(&count, 4, 1, f);
        for (const auto& s : sets) {
            const uint8_t n = static_cast<uint8_t>(s.size());
            std::fwrite(&n, 1, 1, f);
            for (const auto& pk : s) {
                std::fwrite(pk.data(), 1, pk.size(), f);
            }
        }
        std::fclose(f);
        return true;
    }
};

// Block-bundling encoder. Buffers up to N serialized blocks, then writes one
// BDB record per group. Layout per bundle value:
//   [tag:1][version:1=1][count:2 LE][offsets:count*4 LE][block_bytes...]
// tag = 0xB0 raw / 0xB1 LZ4-compressed (everything after the tag is compressed).
// Group key (RECNO) is (first_seq / bundle_size) + 1.
struct Bundler {
    size_t bundle_size = 0;
    bool compress = false;
    csdb::DatabaseBerkeleyDB* db = nullptr;
    std::vector<cs::Bytes> buf;
    uint64_t next_group_id = 0;
    uint64_t bundles_written = 0;
    uint64_t blocks_written = 0;
    uint64_t raw_payload_bytes = 0;   // sum of block bytes (inner payload only)
    uint64_t inner_bundle_bytes = 0;  // sum of bundle-encoded sizes before compression
    uint64_t stored_bytes = 0;        // sum of bytes actually written to BDB

    bool push(cs::Bytes block_bytes) {
        raw_payload_bytes += block_bytes.size();
        buf.push_back(std::move(block_bytes));
        if (buf.size() >= bundle_size) {
            return flush();
        }
        return true;
    }

    bool flush() {
        if (buf.empty()) return true;
        const uint16_t count = static_cast<uint16_t>(buf.size());
        const size_t header_in = 1 /*version*/ + 2 /*count*/ + count * 4 /*offsets*/;
        size_t total_blocks = 0;
        for (const auto& b : buf) total_blocks += b.size();

        cs::Bytes inner;
        inner.reserve(header_in + total_blocks);
        inner.push_back(1);  // version
        inner.push_back(static_cast<uint8_t>(count & 0xFF));
        inner.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
        uint32_t off = static_cast<uint32_t>(header_in);
        for (const auto& b : buf) {
            for (int i = 0; i < 4; ++i) inner.push_back(static_cast<uint8_t>((off >> (i * 8)) & 0xFF));
            off += static_cast<uint32_t>(b.size());
        }
        for (const auto& b : buf) inner.insert(inner.end(), b.begin(), b.end());
        inner_bundle_bytes += inner.size();

        cs::Bytes value;
        if (!compress) {
            value.reserve(1 + inner.size());
            value.push_back(0xB0);
            value.insert(value.end(), inner.begin(), inner.end());
        } else {
            const int bound = LZ4_compressBound(static_cast<int>(inner.size()));
            value.resize(1 + static_cast<size_t>(bound));
            value[0] = 0xB1;
            const int written = LZ4_compress_default(
                reinterpret_cast<const char*>(inner.data()),
                reinterpret_cast<char*>(value.data() + 1),
                static_cast<int>(inner.size()),
                bound);
            if (written <= 0) {
                std::cerr << "bundle: LZ4_compress_default failed (in=" << inner.size() << ")\n";
                return false;
            }
            value.resize(1 + static_cast<size_t>(written));
        }
        stored_bytes += value.size();

        const uint32_t recno_key = static_cast<uint32_t>(next_group_id + 1);
        if (!db->put_recno(recno_key, value)) {
            std::cerr << "bundle: put_recno failed at group=" << next_group_id
                      << ": " << db->last_error_message() << "\n";
            return false;
        }
        ++next_group_id;
        ++bundles_written;
        blocks_written += buf.size();
        buf.clear();
        return true;
    }
};

// Drives the bundling write path end-to-end without using csdb::Storage.
int run_bundling_migration(csdb::Database& src,
                           csdb::DatabaseBerkeleyDB& dst_bdb,
                           const Args& args) {
    auto it = src.new_iterator();
    if (!it) {
        std::cerr << "src iterator unavailable\n";
        return 1;
    }
    if (args.from > 0) it->seek(static_cast<uint32_t>(args.from));
    else it->seek_to_first();

    Bundler bundler;
    bundler.bundle_size = args.bundle_size;
    bundler.compress = args.bundle_compress;
    bundler.db = &dst_bdb;

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto last_log = t0;
    size_t saved = 0;
    uint64_t empty_blocks = 0;
    uint64_t tx_blocks = 0;

    while (it->is_valid()) {
        const uint32_t seq_key = it->key();
        if (static_cast<cs::Sequence>(seq_key) > args.to) break;
        cs::Bytes raw = it->value();
        csdb::Pool pool = csdb::Pool::from_binary(std::move(raw));
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

        cs::Bytes serialized =
            (args.use_stubs && pool.transactions().empty())
                ? csdb::build_empty_pool_stub(pool)
                : pool.to_binary();
        if (!bundler.push(std::move(serialized))) {
            return 1;
        }
        ++saved;
        it->next();

        if (saved % args.progress_every == 0) {
            const auto now = clock::now();
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0);
            const double rate = since_last > 0
                ? (1000.0 * static_cast<double>(args.progress_every) / static_cast<double>(since_last))
                : 0.0;
            std::cout << "  seq=" << seq_key
                      << "  saved=" << saved
                      << "  bundles=" << bundler.bundles_written
                      << "  raw=" << bundler.raw_payload_bytes
                      << "  stored=" << bundler.stored_bytes
                      << "  ratio=" << (bundler.raw_payload_bytes > 0
                          ? (100.0 * static_cast<double>(bundler.stored_bytes) /
                             static_cast<double>(bundler.raw_payload_bytes))
                          : 0.0) << "%"
                      << "  rate=" << static_cast<uint64_t>(rate) << "/s"
                      << "  elapsed=" << format_duration(elapsed)
                      << std::endl;
            last_log = now;
        }
    }

    if (!bundler.flush()) return 1;

    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0);
    std::cout << "bundling done"
              << "  saved=" << saved
              << "  empty=" << empty_blocks
              << "  tx=" << tx_blocks
              << "  bundles=" << bundler.bundles_written
              << "  raw_payload_bytes=" << bundler.raw_payload_bytes
              << "  inner_bundle_bytes=" << bundler.inner_bundle_bytes
              << "  stored_bytes=" << bundler.stored_bytes
              << "  vs_raw=" << (bundler.raw_payload_bytes > 0
                  ? (100.0 * static_cast<double>(bundler.stored_bytes) /
                     static_cast<double>(bundler.raw_payload_bytes)) : 0.0) << "%"
              << "  elapsed=" << format_duration(total)
              << std::endl;
    return 0;
}

// Pubkey dictionary: pubkey -> uint32 wallet-id. Built in pass 1, used in
// pass 2 to rewrite tx source/target and confidants. Dumped as a side file.
struct WalletDict {
    std::map<cs::PublicKey, uint32_t> map;

    uint32_t intern(const cs::PublicKey& pk) {
        auto it = map.find(pk);
        if (it != map.end()) return it->second;
        const uint32_t id = static_cast<uint32_t>(map.size());
        map.emplace(pk, id);
        return id;
    }

    bool dump(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const char magic[4] = {'P','D','I','C'};
        const uint8_t version = 1;
        const uint32_t count = static_cast<uint32_t>(map.size());
        std::fwrite(magic, 1, 4, f);
        std::fwrite(&version, 1, 1, f);
        std::fwrite(&count, 4, 1, f);
        std::vector<const cs::PublicKey*> by_id(map.size(), nullptr);
        for (const auto& kv : map) by_id[kv.second] = &kv.first;
        for (const auto* pk : by_id) {
            std::fwrite(pk->data(), 1, pk->size(), f);
        }
        std::fclose(f);
        return true;
    }
};

// Two-pass remap. PASS 1 walks src and interns every pubkey it sees
// (confidants + tx source + tx target). PASS 2 rewrites and writes — using
// either direct put_recno (one block per record) or the Bundler (N per
// record, optional LZ4) depending on args.bundle_size / args.bundle_compress.
int run_pubkey_remap(csdb::Database& src,
                     csdb::DatabaseBerkeleyDB& dst_bdb,
                     const Args& args) {
    using clock = std::chrono::steady_clock;
    const auto t_total = clock::now();

    // PASS 1
    std::cout << "pubkey-remap pass 1: scanning src to build dict..." << std::endl;
    WalletDict dict;
    {
        auto it = src.new_iterator();
        if (!it) { std::cerr << "src iterator unavailable\n"; return 1; }
        if (args.from > 0) it->seek(static_cast<uint32_t>(args.from));
        else it->seek_to_first();
        uint64_t scanned = 0;
        const auto t0 = clock::now();
        auto last_log = t0;
        while (it->is_valid()) {
            const uint32_t seq_key = it->key();
            if (static_cast<cs::Sequence>(seq_key) > args.to) break;
            cs::Bytes raw = it->value();
            csdb::Pool pool = csdb::Pool::from_binary(std::move(raw));
            if (!pool.is_valid()) { std::cerr << "pass1: src seq=" << seq_key << " unparseable\n"; return 1; }
            for (const auto& pk : pool.confidants()) dict.intern(pk);
            for (const auto& tx : pool.transactions()) {
                const auto& s = tx.source().public_key();
                const auto& t = tx.target().public_key();
                if (s.size() == 32) { cs::PublicKey pk{}; std::memcpy(pk.data(), s.data(), 32); dict.intern(pk); }
                if (t.size() == 32) { cs::PublicKey pk{}; std::memcpy(pk.data(), t.data(), 32); dict.intern(pk); }
            }
            ++scanned;
            it->next();
            if (scanned % args.progress_every == 0) {
                const auto now = clock::now();
                const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
                const double rate = since_last > 0
                    ? (1000.0 * static_cast<double>(args.progress_every) / static_cast<double>(since_last))
                    : 0.0;
                std::cout << "  pass1 seq=" << seq_key
                          << "  scanned=" << scanned
                          << "  unique_pk=" << dict.map.size()
                          << "  rate=" << static_cast<uint64_t>(rate) << "/s"
                          << std::endl;
                last_log = now;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0);
        std::cout << "pass1 done  scanned=" << scanned
                  << "  unique_pubkeys=" << dict.map.size()
                  << "  elapsed=" << format_duration(elapsed) << std::endl;
    }

    // PASS 2
    std::cout << "pubkey-remap pass 2: rewriting + writing dst..." << std::endl;
    Bundler bundler;
    const bool use_bundle = (args.bundle_size > 0);
    if (use_bundle) {
        bundler.bundle_size = args.bundle_size;
        bundler.compress = args.bundle_compress;
        bundler.db = &dst_bdb;
    }

    auto it = src.new_iterator();
    if (!it) { std::cerr << "src iterator unavailable\n"; return 1; }
    if (args.from > 0) it->seek(static_cast<uint32_t>(args.from));
    else it->seek_to_first();

    uint64_t written = 0;
    uint64_t raw_today_bytes = 0;
    uint64_t direct_stored_bytes = 0;  // for non-bundle path
    const auto t0 = clock::now();
    auto last_log = t0;

    while (it->is_valid()) {
        const uint32_t seq_key = it->key();
        if (static_cast<cs::Sequence>(seq_key) > args.to) break;
        cs::Bytes raw = it->value();
        raw_today_bytes += raw.size();
        csdb::Pool pool = csdb::Pool::from_binary(std::move(raw), /*makeReadOnly=*/false);
        if (!pool.is_valid()) { std::cerr << "pass2: src seq=" << seq_key << " unparseable\n"; return 1; }

        auto& txs = pool.transactions();
        for (auto& tx : txs) {
            const auto& s = tx.source().public_key();
            const auto& t = tx.target().public_key();
            if (s.size() == 32) {
                cs::PublicKey pk{}; std::memcpy(pk.data(), s.data(), 32);
                tx.set_source(csdb::Address::from_wallet_id(dict.intern(pk)));
            }
            if (t.size() == 32) {
                cs::PublicKey pk{}; std::memcpy(pk.data(), t.data(), 32);
                tx.set_target(csdb::Address::from_wallet_id(dict.intern(pk)));
            }
        }

        const auto& confidants = pool.confidants();
        if (!confidants.empty()) {
            std::string blob;
            blob.reserve(2 + confidants.size() * 4);
            const uint16_t n = static_cast<uint16_t>(confidants.size());
            blob.push_back(static_cast<char>(n & 0xFF));
            blob.push_back(static_cast<char>((n >> 8) & 0xFF));
            for (const auto& pk : confidants) {
                const uint32_t id = dict.intern(pk);
                for (int i = 0; i < 4; ++i) blob.push_back(static_cast<char>((id >> (i * 8)) & 0xFF));
            }
            pool.set_confidants({});
            pool.add_user_field(kFieldConfidantSetIdExp, blob);
        }

        cs::Bytes encoded = pool.to_binary();
        if (encoded.empty()) {
            std::cerr << "pass2: pool.to_binary() returned empty at seq=" << seq_key << "\n";
            return 1;
        }

        if (use_bundle) {
            if (!bundler.push(std::move(encoded))) return 1;
        } else {
            direct_stored_bytes += encoded.size();
            if (!dst_bdb.put_recno(seq_key + 1, encoded)) {
                std::cerr << "put_recno failed at seq=" << seq_key << ": "
                          << dst_bdb.last_error_message() << "\n";
                return 1;
            }
        }
        ++written;
        it->next();

        if (written % args.progress_every == 0) {
            const auto now = clock::now();
            const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0);
            const double rate = since_last > 0
                ? (1000.0 * static_cast<double>(args.progress_every) / static_cast<double>(since_last))
                : 0.0;
            const uint64_t stored_so_far = use_bundle ? bundler.stored_bytes : direct_stored_bytes;
            std::cout << "  pass2 seq=" << seq_key
                      << "  written=" << written
                      << "  raw=" << raw_today_bytes
                      << "  stored=" << stored_so_far
                      << "  ratio=" << (raw_today_bytes > 0
                          ? 100.0 * static_cast<double>(stored_so_far) / static_cast<double>(raw_today_bytes)
                          : 0.0) << "%"
                      << "  rate=" << static_cast<uint64_t>(rate) << "/s"
                      << "  elapsed=" << format_duration(elapsed)
                      << std::endl;
            last_log = now;
        }
    }
    if (use_bundle && !bundler.flush()) return 1;

    const std::string dict_path = (fs::path(args.dst) / "pubkey_dict.bin").string();
    if (!dict.dump(dict_path)) {
        std::cerr << "failed to dump pubkey_dict.bin\n";
        return 1;
    }
    const uint64_t dict_bytes = 4 + 1 + 4 + dict.map.size() * 32;
    const uint64_t final_stored = use_bundle ? bundler.stored_bytes : direct_stored_bytes;
    const uint64_t plus_dict   = final_stored + dict_bytes;
    const auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t_total);

    std::cout << "pubkey-remap done\n"
              << "  blocks_written        = " << written << "\n"
              << "  unique_pubkeys        = " << dict.map.size() << "\n"
              << "  raw_today_bytes       = " << raw_today_bytes
              << "  (" << (raw_today_bytes / (1024.0 * 1024.0)) << " MiB)\n"
              << "  stored_payload_bytes  = " << final_stored
              << "  (" << (final_stored / (1024.0 * 1024.0)) << " MiB)\n"
              << "  pubkey_dict_bytes     = " << dict_bytes
              << "  (" << (dict_bytes / (1024.0 * 1024.0)) << " MiB)  -> " << dict_path << "\n"
              << "  stored + dict         = " << plus_dict
              << "  (" << (plus_dict / (1024.0 * 1024.0)) << " MiB)\n"
              << "  vs_raw                = " << (raw_today_bytes > 0
                  ? 100.0 * static_cast<double>(plus_dict) / static_cast<double>(raw_today_bytes)
                  : 0.0) << "%\n"
              << "  bundle_size           = " << args.bundle_size << "\n"
              << "  bundle_compress       = " << (args.bundle_compress ? "lz4" : "off") << "\n"
              << "  elapsed               = " << format_duration(total_elapsed) << "\n"
              << "  note: stored_bytes is the BDB record payload; on-disk size also\n"
              << "        includes BDB page padding — compare actual dst folder size\n"
              << "        against a baseline run (no --pubkey-remap, same other flags).\n";
    return 0;
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
        // open() is on each concrete DB class, not the abstract base, so call
        // through the typed handle before storing in the shared_ptr<Database>.
        std::shared_ptr<csdb::Database> v_dst;
        if (args.dst_backend == "berkeley") {
            auto v_dst_typed = std::make_shared<csdb::DatabaseBerkeleyDB>();
            if (!v_dst_typed->open(args.dst)) {
                std::cerr << "failed to open dst (berkeley) at " << args.dst
                          << ": " << v_dst_typed->last_error_message() << "\n";
                return 1;
            }
            v_dst = std::move(v_dst_typed);
        } else {
            auto v_dst_typed = std::make_shared<csdb::DatabaseRocksDB>();
            if (!v_dst_typed->open(args.dst)) {
                std::cerr << "failed to open dst (rocksdb) at " << args.dst
                          << ": " << v_dst_typed->last_error_message() << "\n";
                return 1;
            }
            v_dst = std::move(v_dst_typed);
        }
        csdb::Database& v_src_base = v_src;
        csdb::Database& v_dst_base = *v_dst;
        bool ok = validate_chain(v_src_base, v_dst_base, args);
        if (ok && args.verify_balances) {
            ok = verify_balances(v_src_base, v_dst_base, args);
        }
        return ok ? 0 : 1;
    }
    if (args.confidant_indirection && args.dst_backend != "berkeley") {
        std::cerr << "--confidant-indirection currently requires --dst-backend berkeley "
                     "(the produced dst is a measurement artifact, not node-loadable)\n";
        return 2;
    }
    if (args.bundle_size > 0 && args.dst_backend != "berkeley") {
        std::cerr << "--bundle-size currently requires --dst-backend berkeley\n";
        return 2;
    }
    if (args.bundle_size > 0 && args.confidant_indirection) {
        std::cerr << "--bundle-size and --confidant-indirection are mutually exclusive\n";
        return 2;
    }
    if (args.pubkey_remap && args.dst_backend != "berkeley") {
        std::cerr << "--pubkey-remap currently requires --dst-backend berkeley\n";
        return 2;
    }
    if (args.pubkey_remap && args.confidant_indirection) {
        std::cerr << "--pubkey-remap and --confidant-indirection both use user_field 99; pick one\n";
        return 2;
    }
    if (!args.force && !dst_dir_empty(args.dst)) {
        std::cerr << "dst is not empty: " << args.dst
                  << "\n  Pass --force to override (note: chain check requires a fresh dst).\n";
        return 2;
    }
    fs::create_directories(args.dst);
    if (args.confidant_indirection && args.confidant_sets_out.empty()) {
        args.confidant_sets_out = (fs::path(args.dst) / "confidant_sets.bin").string();
    }

    csdb::DatabaseBerkeleyDB src_db;
    if (!src_db.open(args.src)) {
        std::cerr << "failed to open BerkeleyDB at " << args.src
                  << ": " << src_db.last_error_message() << "\n";
        return 1;
    }

    // Polymorphic destination: keep a base shared_ptr for Storage and a typed
    // pointer to the RocksDB instance (when applicable) for the bulk-load /
    // compact_full hooks that don't exist on BerkeleyDB. open() is on each
    // concrete class — call it through the typed handle before storing in the
    // base shared_ptr.
    std::shared_ptr<csdb::Database> dst_db;
    csdb::DatabaseRocksDB* dst_rocks = nullptr;
    csdb::DatabaseBerkeleyDB* dst_bdb = nullptr;
    if (args.dst_backend == "berkeley") {
        auto bdb = std::make_shared<csdb::DatabaseBerkeleyDB>();
        // 256 MB mpool + 64 MB in-memory log; no on-disk log files.
        bdb->tune_for_bulk(256ULL * 1024 * 1024, 64u * 1024 * 1024);
        if (args.bulk_load) {
            std::cout << "note: --dst-backend berkeley uses in-memory log instead of bulk_load\n";
        }
        if (!bdb->open(args.dst)) {
            std::cerr << "failed to open dst (berkeley) at " << args.dst
                      << ": " << bdb->last_error_message() << "\n";
            return 1;
        }
        dst_bdb = bdb.get();
        dst_db = std::move(bdb);
    } else {
        auto rocks = std::make_shared<csdb::DatabaseRocksDB>();
        if (args.bulk_load) {
            rocks->set_bulk_load(true);
        }
        if (!rocks->open(args.dst)) {
            std::cerr << "failed to open dst (rocksdb) at " << args.dst
                      << ": " << rocks->last_error_message() << "\n";
            return 1;
        }
        dst_rocks = rocks.get();
        dst_db = std::move(rocks);
    }

    if (args.pubkey_remap) {
        std::cout << "csdb_migrate (PUBKEY-REMAP)"
                  << "\n  src: "       << args.src
                  << "\n  dst: "       << args.dst
                  << "\n  from: "      << args.from
                  << "\n  to: "        << (args.to == std::numeric_limits<cs::Sequence>::max() ? std::string("end") : std::to_string(args.to))
                  << "\n  bundle-size: " << args.bundle_size
                  << "\n  bundle-compress: " << (args.bundle_compress ? "lz4" : "off")
                  << "\n  note: dst is NOT node-loadable; emits pubkey_dict.bin"
                  << std::endl;
        csdb::Database& src_base_p = src_db;
        return run_pubkey_remap(src_base_p, *dst_bdb, args);
    }
    if (args.bundle_size > 0) {
        std::cout << "csdb_migrate (BUNDLING)"
                  << "\n  src: "       << args.src
                  << "\n  dst: "       << args.dst
                  << "\n  from: "      << args.from
                  << "\n  to: "        << (args.to == std::numeric_limits<cs::Sequence>::max() ? std::string("end") : std::to_string(args.to))
                  << "\n  bundle-size: " << args.bundle_size
                  << "\n  bundle-compress: " << (args.bundle_compress ? "lz4" : "off")
                  << "\n  stubs: "     << (args.use_stubs ? "enabled" : "disabled (full pools)")
                  << "\n  note: dst is NOT node-loadable"
                  << std::endl;
        csdb::Database& src_base_b = src_db;
        return run_bundling_migration(src_base_b, *dst_bdb, args);
    }

    csdb::Storage::OpenOptions opt;
    opt.db = dst_db;
    opt.newBlockchainTop = cs::kWrongSequence;
    opt.startSequence = 0;
    opt.asyncWriteQueueMax = args.queue;
    opt.writeBatchSize = args.batch;
    opt.useEmptyPoolStubs = args.use_stubs;

    csdb::Storage dst;
    if (!dst.open(opt)) {
        std::cerr << "failed to open destination Storage: "
                  << dst.last_error_message() << "\n";
        return 1;
    }

    std::cout << "csdb_migrate"
              << "\n  src: "       << args.src
              << "\n  dst: "       << args.dst
              << "\n  dst-backend: " << args.dst_backend
              << "\n  from: "      << args.from
              << "\n  to: "        << (args.to == std::numeric_limits<cs::Sequence>::max() ? std::string("end") : std::to_string(args.to))
              << "\n  queue: "     << args.queue
              << "\n  batch: "     << args.batch
              << "\n  progress: "  << args.progress_every
              << "\n  stubs: "     << (args.use_stubs ? "enabled" : "disabled (full pools)")
              << "\n  confidant-indirection: " << (args.confidant_indirection ? "ENABLED (dst NOT node-loadable)" : "disabled")
              << std::endl;

    csdb::Database& src_base = src_db;
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

    ConfidantInterner interner;

    while (it->is_valid()) {
        const auto ts_a = clock::now();
        const uint32_t seq_key = it->key();
        if (static_cast<cs::Sequence>(seq_key) > args.to) break;
        cs::Bytes raw = it->value();
        const auto ts_b = clock::now();

        csdb::Pool pool = csdb::Pool::from_binary(std::move(raw),
                                                  /*makeReadOnly=*/!args.confidant_indirection);
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

        if (args.confidant_indirection) {
            const uint32_t set_id = interner.intern(pool.confidants());
            pool.set_confidants({});
            pool.add_user_field(kFieldConfidantSetIdExp,
                                static_cast<uint64_t>(set_id));
        }

        if (!dst.pool_save(pool)) {
            std::cerr << "dst pool_save failed at seq=" << seq_key << ": "
                      << dst.last_error_message()
                      << " (db: " << dst.db_last_error_message() << ")\n";
            return 1;
        }
        const auto ts_d = clock::now();
        ++saved;

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

    if (args.bulk_load && dst_rocks != nullptr) {
        std::cout << "compacting destination (bulk-load mode) ..." << std::endl;
        const auto t_compact = clock::now();
        if (!dst_rocks->compact_full()) {
            std::cerr << "compact_full failed: " << dst_rocks->last_error_message() << "\n";
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

    if (args.confidant_indirection) {
        if (!interner.dump(args.confidant_sets_out)) {
            std::cerr << "confidant-indirection: failed to write side file "
                      << args.confidant_sets_out << "\n";
            return 1;
        }
        // Per-block ref cost: user_field 99 of integer type. obstream writes
        // [id:varint ~2B][type:1B][value:varint ~1-5B] -> typically ~5-7 bytes
        // and the enclosing user_fields map gains 1B for its count byte going
        // from 0 to 1 (only when the block had no other user fields).
        const uint64_t per_block_ref_bytes = 6;
        const uint64_t inline_today = interner.inline_bytes_today;
        const uint64_t per_block_new = interner.blocks_with_conf * per_block_ref_bytes;
        const uint64_t table_bytes = interner.table_bytes();
        const int64_t  net_savings  = static_cast<int64_t>(inline_today) -
                                      static_cast<int64_t>(per_block_new) -
                                      static_cast<int64_t>(table_bytes);
        std::cout << "confidant-indirection summary:\n"
                  << "  blocks_with_conf    = " << interner.blocks_with_conf << "\n"
                  << "  blocks_empty_conf   = " << interner.blocks_empty_conf << "\n"
                  << "  unique_sets         = " << interner.sets.size() << "\n"
                  << "  reuse_factor        = "
                  << (interner.sets.empty() ? 0.0
                          : static_cast<double>(interner.blocks_with_conf) /
                            static_cast<double>(interner.sets.size()))
                  << "x\n"
                  << "  inline_today_bytes  = " << inline_today << "\n"
                  << "  per_block_ref_bytes = " << per_block_new
                  << "  (~" << per_block_ref_bytes << " B/block, see note in code)\n"
                  << "  set_table_bytes     = " << table_bytes
                  << "  (side file: " << args.confidant_sets_out << ")\n"
                  << "  projected_savings   = " << net_savings << " bytes ("
                  << (net_savings / (1024.0 * 1024.0)) << " MiB)\n"
                  << "  note: actual on-disk delta also depends on BDB page packing;\n"
                  << "        compare the dst directory size against a baseline run\n"
                  << "        with --no-stubs vs --confidant-indirection.\n";
    }

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
        if (args.confidant_indirection) {
            std::cout << "cross-verify: skipped (--confidant-indirection mutates blocks; "
                         "dst confidants are empty by design)" << std::endl;
        } else {
            std::cout << "cross-verify: comparing src vs dst per-block ..." << std::endl;
            csdb::Database& cv_src = src_db;
            csdb::Database& cv_dst = *dst_db;
            if (!validate_chain(cv_src, cv_dst, args)) {
                return 1;
            }
        }
    }

    if (args.verify_balances) {
        std::cout << "verify-balances: replaying both DBs through native-coin accounting ..." << std::endl;
        csdb::Database& vb_src = src_db;
        csdb::Database& vb_dst = *dst_db;
        if (!verify_balances(vb_src, vb_dst, args)) {
            return 1;
        }
    }

    return 0;
}
