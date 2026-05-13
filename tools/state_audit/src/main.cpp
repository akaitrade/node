// state_audit: offline forensic validator for a CREDITS node data directory.
//
// Phase 1 (qs/):     structural validation only — schema, head.bin magic,
//                    sentinel, presence + non-emptiness of expected .dat files,
//                    parse-ability of head.bin and quick_start_hashes.dat.
//                    Hash content validation is *not* done here because
//                    several serializers' hash() functions don't match the
//                    on-disk file bytes; the node validates that itself on
//                    startup (proven by presence of qs/N.bad/ quarantines).
// Phase 2 (caches/): open seqdb/hashdb/indexdb/poolcachedb LMDBs read-only,
//                    dump stats, cross-validate seqdb<->hashdb inversion on
//                    a sample of keys, compare seqdb max sequence to chain
//                    db head.
// Phase 3 (db/):     full chain prev_hash integrity walk via the existing
//                    cs::chain_integrity API.
//
// All phases run; each reports independently. Exit code 0 iff every phase
// passes. Read-only, never modifies any file.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <csdb/database.hpp>
#include <csdb/pool.hpp>
#include <csdb/transaction.hpp>
#include <csnode/chain_integrity.hpp>

#include <lmdb++.h>

namespace fs = std::filesystem;

namespace {

// Tee streambuf: writes every byte to two underlying buffers.
// Used to mirror std::cout / std::cerr to a log file simultaneously.
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* a, std::streambuf* b) : a_(a), b_(b) {}
protected:
    int sync() override {
        const int ra = a_->pubsync();
        const int rb = b_->pubsync();
        return (ra == 0 && rb == 0) ? 0 : -1;
    }
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        const int_type ra = a_->sputc(static_cast<char>(c));
        const int_type rb = b_->sputc(static_cast<char>(c));
        return (ra == traits_type::eof() || rb == traits_type::eof()) ? traits_type::eof() : c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        const auto ra = a_->sputn(s, n);
        const auto rb = b_->sputn(s, n);
        return std::min(ra, rb);
    }
private:
    std::streambuf* a_;
    std::streambuf* b_;
};

// ===== Phase 1 helpers ======================================================

constexpr uint32_t kHeadMagic = 0x44414548u;  // "HEAD" LE
constexpr uint8_t  kSentinelCompletedFromGenesisBit = 1 << 0;
constexpr uint8_t  kCurrentSchemaVersion = 1;

const std::vector<std::pair<std::string, std::string>> kSections = {
    {"blockchain",     "blockchain.dat"},
    {"smartcontracts", "smartcontracts.dat"},
    {"walletscache",   "walletscache.dat"},
    {"walletsIds",     "walletsids.dat"},
    {"roundstat",      "roundstat.dat"},
    {"tokensmaster",   "tokens.dat"},
    {"apihandler",     "apihandler.dat"},
};

struct Head {
    uint64_t sequence = 0;
    std::vector<uint8_t> head_hash;
    std::vector<uint8_t> prev_hash;
};

bool readSchema(const fs::path& dir, uint8_t& schema) {
    std::ifstream f(dir / "schema_version", std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(&schema), sizeof(schema));
    return static_cast<bool>(f);
}

bool readHead(const fs::path& dir, Head& out) {
    std::ifstream f(dir / "head.bin", std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!f || magic != kHeadMagic) return false;
    f.read(reinterpret_cast<char*>(&out.sequence), sizeof(out.sequence));
    uint16_t hh = 0, ph = 0;
    f.read(reinterpret_cast<char*>(&hh), sizeof(hh));
    if (hh) { out.head_hash.resize(hh); f.read(reinterpret_cast<char*>(out.head_hash.data()), hh); }
    f.read(reinterpret_cast<char*>(&ph), sizeof(ph));
    if (ph) { out.prev_hash.resize(ph); f.read(reinterpret_cast<char*>(out.prev_hash.data()), ph); }
    return static_cast<bool>(f);
}

bool readSentinel(const fs::path& dir, bool& completed) {
    std::ifstream f(dir / "sentinel.bin", std::ios::binary);
    if (!f) return false;
    uint8_t flags = 0;
    f.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    completed = (flags & kSentinelCompletedFromGenesisBit) != 0;
    return static_cast<bool>(f);
}

bool readStoredHashes(const fs::path& dir, size_t& sectionsCount) {
    std::ifstream f(dir / "quick_start_hashes.dat");
    if (!f) return false;
    std::string content;
    f >> content;
    while (!content.empty() && content.back() == '\0') content.pop_back();
    constexpr size_t hexLen = 64;
    if (content.empty() || content.size() % hexLen != 0) return false;
    sectionsCount = content.size() / hexLen;
    return true;
}

std::string bytesToHex(const std::vector<uint8_t>& v) {
    static const char* kHex = "0123456789abcdef";
    std::string s(v.size() * 2, '0');
    for (size_t i = 0; i < v.size(); ++i) {
        s[i*2]   = kHex[(v[i] >> 4) & 0xF];
        s[i*2+1] = kHex[v[i] & 0xF];
    }
    return s;
}

std::set<size_t> enumerateVersions(const fs::path& qsDir) {
    std::set<size_t> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(qsDir, ec)) {
        if (!entry.is_directory()) continue;
        const auto ext = entry.path().extension().string();
        if (ext == ".tmp" || ext == ".bad" || ext == ".prev") continue;
        const auto name = entry.path().filename().string();
        try {
            result.insert(std::stoull(name));
        } catch (...) {
        }
    }
    return result;
}

int auditQuickStart(const fs::path& dataDir) {
    std::cout << "\n========== PHASE 1: qs/ checkpoint structure ==========\n";
    fs::path qsDir = dataDir / "qs";
    if (!fs::exists(qsDir) || !fs::is_directory(qsDir)) {
        std::cout << "SKIP: " << qsDir.string() << " not a directory\n";
        return 0;  // not an error — just nothing to check
    }
    auto versions = enumerateVersions(qsDir);
    if (versions.empty()) {
        std::cout << "WARN: no qs/N/ checkpoints found in " << qsDir.string() << "\n";
        return 0;
    }

    // List quarantined for the operator's record.
    std::vector<std::string> quarantined;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(qsDir, ec)) {
        if (entry.is_directory() && entry.path().extension() == ".bad") {
            quarantined.push_back(entry.path().filename().string());
        }
    }
    if (!quarantined.empty()) {
        std::cout << "Quarantined (.bad) dirs found (node previously rejected these): ";
        for (auto& q : quarantined) std::cout << q << " ";
        std::cout << "\n";
    }

    int failed = 0;
    for (size_t v : versions) {
        fs::path p = qsDir / std::to_string(v);
        std::cout << "\n--- qs/" << v << " ---\n";

        // schema_version
        uint8_t schema = 0;
        if (!readSchema(p, schema)) {
            std::cout << "  schema_version : MISSING (legacy)\n";
        } else if (schema != kCurrentSchemaVersion) {
            std::cout << "  schema_version : FAIL (got " << int(schema)
                      << ", expected " << int(kCurrentSchemaVersion) << ")\n";
            ++failed;
            continue;
        } else {
            std::cout << "  schema_version : OK (" << int(schema) << ")\n";
        }

        // head.bin
        Head head;
        if (!readHead(p, head)) {
            std::cout << "  head.bin       : FAIL (missing or bad magic)\n";
            ++failed;
            continue;
        }
        std::cout << "  head.bin       : OK  seq=" << head.sequence
                  << "  head_hash=" << (head.head_hash.empty() ? "<empty>" : bytesToHex(head.head_hash))
                  << "  prev_hash=" << (head.prev_hash.empty() ? "<empty>" : bytesToHex(head.prev_hash))
                  << "\n";

        // sentinel.bin
        bool completed = false;
        if (!readSentinel(p, completed)) {
            std::cout << "  sentinel.bin   : MISSING (legacy)\n";
        } else {
            std::cout << "  sentinel.bin   : OK  completedFromGenesis="
                      << (completed ? "true" : "false") << "\n";
        }

        // quick_start_hashes.dat
        size_t sections = 0;
        if (!readStoredHashes(p, sections)) {
            std::cout << "  hashes file    : FAIL (missing or malformed)\n";
            ++failed;
            continue;
        }
        std::cout << "  hashes file    : OK  (" << sections << " section hashes)\n";

        // Files present + non-empty
        bool anyMissing = false;
        for (const auto& [label, file] : kSections) {
            fs::path datFile = p / file;
            std::error_code ec2;
            if (!fs::exists(datFile, ec2)) {
                std::cout << "  " << file << " : FAIL (missing)\n";
                anyMissing = true;
                continue;
            }
            auto sz = fs::file_size(datFile, ec2);
            if (ec2) {
                std::cout << "  " << file << " : FAIL (stat error)\n";
                anyMissing = true;
                continue;
            }
            if (sz == 0) {
                std::cout << "  " << file << " : FAIL (empty file)\n";
                anyMissing = true;
                continue;
            }
            std::cout << "  " << file << " : present (" << sz << " bytes)\n";
        }
        if (anyMissing) ++failed;
    }

    std::cout << "\nPhase 1 summary: " << versions.size() << " checkpoint(s) scanned, "
              << failed << " structural failure(s).\n"
              << "Note: per-section hash validation is performed by the node at startup,\n"
              << "and is signalled by qs/N.bad/ quarantines (none means the node accepted\n"
              << "these checkpoints).\n";
    return failed > 0 ? 1 : 0;
}

// ===== Phase 2: caches/ LMDB =================================================

struct LmdbStats {
    bool ok = false;
    std::string err;
    size_t entries = 0;
    cs::Sequence minSeq = 0;
    cs::Sequence maxSeq = 0;
    std::vector<std::pair<cs::Sequence, std::vector<uint8_t>>> samples;  // for seqdb
};

// Open an LMDB env read-only and call `body` with a read txn + dbi.
template <typename Body>
LmdbStats openReadOnly(const fs::path& dir, Body body) {
    LmdbStats stats;
    try {
        auto env = lmdb::env::create();
        env.set_mapsize(static_cast<size_t>(64) * 1024 * 1024 * 1024);  // 64 GiB; LMDB only reserves vm, not physical
        env.open(dir.string().c_str(), MDB_RDONLY | MDB_NOLOCK, 0644);
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        body(txn, dbi, stats);
        txn.abort();
        stats.ok = true;
    } catch (const lmdb::error& e) {
        stats.err = std::string("lmdb error: ") + e.what();
    } catch (const std::exception& e) {
        stats.err = std::string("error: ") + e.what();
    } catch (...) {
        stats.err = "unknown error";
    }
    return stats;
}

LmdbStats inspectSeqDb(const fs::path& dir) {
    return openReadOnly(dir, [&](lmdb::txn& txn, lmdb::dbi& dbi, LmdbStats& stats) {
        stats.entries = dbi.size(txn);
        if (stats.entries == 0) return;
        auto cur = lmdb::cursor::open(txn, dbi);
        lmdb::val key, val;
        if (cur.get(key, val, MDB_FIRST)) {
            if (key.size() == sizeof(cs::Sequence)) {
                std::memcpy(&stats.minSeq, key.data(), sizeof(cs::Sequence));
            }
            std::vector<uint8_t> h(reinterpret_cast<const uint8_t*>(val.data()),
                                   reinterpret_cast<const uint8_t*>(val.data()) + val.size());
            stats.samples.emplace_back(stats.minSeq, std::move(h));
        }
        if (cur.get(key, val, MDB_LAST)) {
            if (key.size() == sizeof(cs::Sequence)) {
                std::memcpy(&stats.maxSeq, key.data(), sizeof(cs::Sequence));
            }
            std::vector<uint8_t> h(reinterpret_cast<const uint8_t*>(val.data()),
                                   reinterpret_cast<const uint8_t*>(val.data()) + val.size());
            stats.samples.emplace_back(stats.maxSeq, std::move(h));
        }
        // 100 random samples (skip if too few entries)
        if (stats.maxSeq > stats.minSeq + 100) {
            std::mt19937_64 rng(42);
            std::uniform_int_distribution<cs::Sequence> dist(stats.minSeq, stats.maxSeq);
            for (int i = 0; i < 100; ++i) {
                cs::Sequence seq = dist(rng);
                lmdb::val k(reinterpret_cast<const void*>(&seq), sizeof(seq));
                lmdb::val v;
                if (dbi.get(txn, k, v)) {
                    std::vector<uint8_t> h(reinterpret_cast<const uint8_t*>(v.data()),
                                           reinterpret_cast<const uint8_t*>(v.data()) + v.size());
                    stats.samples.emplace_back(seq, std::move(h));
                }
            }
        }
        cur.close();
    });
}

LmdbStats inspectGenericLmdb(const fs::path& dir) {
    return openReadOnly(dir, [&](lmdb::txn& txn, lmdb::dbi& dbi, LmdbStats& stats) {
        stats.entries = dbi.size(txn);
    });
}

// Verify hashdb[H] == seq for each (seq, H) sample from seqdb.
bool verifyInversion(const fs::path& hashDir, const LmdbStats& seqStats, size_t& matched, size_t& mismatched) {
    matched = 0;
    mismatched = 0;
    bool opened = false;
    try {
        auto env = lmdb::env::create();
        env.set_mapsize(static_cast<size_t>(64) * 1024 * 1024 * 1024);
        env.open(hashDir.string().c_str(), MDB_RDONLY | MDB_NOLOCK, 0644);
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        opened = true;
        for (const auto& [seq, hash] : seqStats.samples) {
            lmdb::val k(hash.data(), hash.size());
            lmdb::val v;
            if (!dbi.get(txn, k, v)) {
                ++mismatched;
                continue;
            }
            if (v.size() != sizeof(cs::Sequence)) {
                ++mismatched;
                continue;
            }
            cs::Sequence got = 0;
            std::memcpy(&got, v.data(), sizeof(cs::Sequence));
            if (got == seq) ++matched; else ++mismatched;
        }
        txn.abort();
    } catch (...) {
        if (!opened) {
            std::cerr << "    (could not open hashdb)\n";
        }
        return false;
    }
    return true;
}

// ---- helpers for expanded Phase 2 cross-DB checks ----

struct IndexdbMeta {
    bool ok = false;
    std::string err;
    bool hasLastIndexed = false;
    cs::Sequence lastIndexed = 0;
    bool hasIncomplete = false;
    uint8_t incomplete = 0;
    bool hasWalkedComplete = false;
    uint8_t walkedComplete = 0;
    size_t totalEntries = 0;
};

IndexdbMeta readIndexdbMeta(const fs::path& dir) {
    IndexdbMeta m;
    try {
        auto env = lmdb::env::create();
        env.set_mapsize(static_cast<size_t>(64) * 1024 * 1024 * 1024);
        env.open(dir.string().c_str(), MDB_RDONLY | MDB_NOLOCK, 0644);
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        m.totalEntries = dbi.size(txn);
        auto rd = [&](const char* keyStr, auto handler) {
            lmdb::val k(keyStr, std::strlen(keyStr));
            lmdb::val v;
            if (dbi.get(txn, k, v)) handler(v);
        };
        rd("__last_indexed__", [&](lmdb::val& v){
            if (v.size() == sizeof(cs::Sequence)) {
                std::memcpy(&m.lastIndexed, v.data(), sizeof(cs::Sequence));
                m.hasLastIndexed = true;
            }
        });
        rd("__incomplete__", [&](lmdb::val& v){
            if (v.size() == 1) { m.incomplete = uint8_t(*reinterpret_cast<const char*>(v.data())); m.hasIncomplete = true; }
        });
        rd("__walked_complete__", [&](lmdb::val& v){
            if (v.size() == 1) { m.walkedComplete = uint8_t(*reinterpret_cast<const char*>(v.data())); m.hasWalkedComplete = true; }
        });
        txn.abort();
        m.ok = true;
    } catch (const std::exception& e) { m.err = e.what(); }
      catch (...) { m.err = "unknown error"; }
    return m;
}

void crossCheckSeqdbVsChain(csdb::Database& db, const LmdbStats& seqStats,
                            size_t& matched, size_t& mismatched, size_t& missingInDb,
                            size_t printLimit) {
    matched = mismatched = missingInDb = 0;
    size_t printed = 0;
    for (const auto& [seq, hash_cache] : seqStats.samples) {
        cs::Bytes poolBytes;
        if (!db.get(static_cast<uint32_t>(seq + 1), &poolBytes) || poolBytes.empty()) {
            ++missingInDb;
            continue;
        }
        csdb::Pool pool = csdb::Pool::from_binary(std::move(poolBytes));
        if (!pool.is_valid()) { ++missingInDb; continue; }
        const auto db_hash_bytes = pool.hash().to_binary();
        if (db_hash_bytes.size() == hash_cache.size() &&
            std::equal(db_hash_bytes.begin(), db_hash_bytes.end(), hash_cache.begin())) {
            ++matched;
        } else {
            ++mismatched;
            if (printed < printLimit) {
                std::cout << "    seq " << seq << ": seqdb=" << bytesToHex(hash_cache)
                          << "  chain=" << bytesToHex(db_hash_bytes) << "\n";
                ++printed;
            }
        }
    }
}

std::vector<uint8_t> makeTrxIndexKey(const std::vector<uint8_t>& pubkey, cs::Sequence seq) {
    std::vector<uint8_t> k;
    k.reserve(pubkey.size() + sizeof(seq));
    k.insert(k.end(), pubkey.begin(), pubkey.end());
    auto p = reinterpret_cast<const uint8_t*>(&seq);
    k.insert(k.end(), p, p + sizeof(seq));
    return k;
}

void crossCheckIndexdbAgainstChain(const fs::path& indexDir, csdb::Database& db,
                                   cs::Sequence chainHead, size_t sampleBlocks,
                                   size_t& checkedBlocks, size_t& presentKeys,
                                   size_t& absentKeys) {
    checkedBlocks = presentKeys = absentKeys = 0;
    try {
        auto env = lmdb::env::create();
        env.set_mapsize(static_cast<size_t>(64) * 1024 * 1024 * 1024);
        env.open(indexDir.string().c_str(), MDB_RDONLY | MDB_NOLOCK, 0644);
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        std::mt19937_64 rng(7);
        std::uniform_int_distribution<cs::Sequence> dist(1, chainHead);
        size_t tries = 0;
        const size_t maxTries = sampleBlocks * 20;
        size_t printed = 0;
        while (checkedBlocks < sampleBlocks && tries < maxTries) {
            ++tries;
            cs::Sequence seq = dist(rng);
            cs::Bytes poolBytes;
            if (!db.get(static_cast<uint32_t>(seq + 1), &poolBytes)) continue;
            if (poolBytes.empty()) continue;
            csdb::Pool pool = csdb::Pool::from_binary(std::move(poolBytes));
            if (!pool.is_valid()) continue;
            if (pool.transactions_count() == 0) continue;
            const auto& txs = pool.transactions();
            auto src = txs.front().source();
            auto srcKey = src.public_key();
            if (srcKey.empty()) continue;
            std::vector<uint8_t> pubkey(srcKey.begin(), srcKey.end());
            if (pubkey.size() != 32) continue;
            auto key = makeTrxIndexKey(pubkey, seq);
            lmdb::val k(key.data(), key.size());
            lmdb::val v;
            ++checkedBlocks;
            if (dbi.get(txn, k, v)) ++presentKeys;
            else {
                ++absentKeys;
                if (printed < 5) {
                    std::cout << "    seq " << seq << " sender="
                              << bytesToHex(pubkey).substr(0, 16) << "...  NOT in indexdb\n";
                    ++printed;
                }
            }
        }
        txn.abort();
    } catch (const std::exception& e) {
        std::cout << "    (indexdb cross-check threw: " << e.what() << ")\n";
    }
}

int auditCaches(const fs::path& dataDir, const std::string& backend, cs::Sequence chainHead /* 0 = unknown */) {
    std::cout << "\n========== PHASE 2: caches/ LMDB consistency ==========\n";
    fs::path cachesDir = dataDir / "caches";
    if (!fs::exists(cachesDir) || !fs::is_directory(cachesDir)) {
        std::cout << "SKIP: " << cachesDir.string() << " not a directory\n";
        return 0;
    }

    int failed = 0;

    auto report = [&](const char* name, const fs::path& dir, const LmdbStats& s) {
        std::cout << "  " << name << " (" << dir.filename().string() << "): ";
        if (!s.ok) {
            std::cout << "FAIL — " << s.err << "\n";
            ++failed;
            return;
        }
        std::cout << "OK  entries=" << s.entries;
        if (s.entries > 0 && (s.minSeq != 0 || s.maxSeq != 0)) {
            std::cout << "  min_seq=" << s.minSeq << "  max_seq=" << s.maxSeq;
        }
        std::cout << "\n";
    };

    fs::path seqDir = cachesDir / "seqdb";
    fs::path hashDir = cachesDir / "hashdb";
    fs::path indexDir = cachesDir / "indexdb";
    fs::path poolDir = cachesDir / "poolcachedb";

    LmdbStats seqStats = fs::exists(seqDir) ? inspectSeqDb(seqDir) : LmdbStats{};
    if (fs::exists(seqDir)) report("seqdb",       seqDir, seqStats); else std::cout << "  seqdb       : MISSING\n";

    LmdbStats hashStats = fs::exists(hashDir) ? inspectGenericLmdb(hashDir) : LmdbStats{};
    if (fs::exists(hashDir)) report("hashdb",     hashDir, hashStats); else std::cout << "  hashdb      : MISSING\n";

    LmdbStats idxStats = fs::exists(indexDir) ? inspectGenericLmdb(indexDir) : LmdbStats{};
    if (fs::exists(indexDir)) report("indexdb",   indexDir, idxStats); else std::cout << "  indexdb     : MISSING\n";

    LmdbStats poolStats = fs::exists(poolDir) ? inspectGenericLmdb(poolDir) : LmdbStats{};
    if (fs::exists(poolDir)) report("poolcachedb", poolDir, poolStats); else std::cout << "  poolcachedb : MISSING\n";

    // Cross-validation: seqdb<->hashdb inversion
    if (seqStats.ok && hashStats.ok && !seqStats.samples.empty()) {
        std::cout << "\n  Cross-check seqdb -> hashdb on " << seqStats.samples.size()
                  << " samples...\n";
        size_t matched = 0, mismatched = 0;
        if (verifyInversion(hashDir, seqStats, matched, mismatched)) {
            std::cout << "    matched=" << matched << "  mismatched=" << mismatched;
            if (mismatched > 0) {
                std::cout << "  FAIL\n";
                ++failed;
            } else {
                std::cout << "  OK\n";
            }
        } else {
            std::cout << "    inversion check threw — FAIL\n";
            ++failed;
        }
    }

    // Cross-check seqdb maxSeq vs chain head
    if (seqStats.ok && chainHead != 0) {
        std::cout << "\n  seqdb.max=" << seqStats.maxSeq
                  << "  db.chainHead=" << chainHead;
        if (seqStats.maxSeq == chainHead) {
            std::cout << "  OK (aligned)\n";
        } else if (seqStats.maxSeq > chainHead) {
            std::cout << "  FAIL (seqdb has " << (seqStats.maxSeq - chainHead)
                      << " entries beyond chain head — STALE INDEX from before chain trim)\n";
            ++failed;
        } else {
            std::cout << "  WARN (seqdb is " << (chainHead - seqStats.maxSeq)
                      << " behind chain — index not fully rebuilt)\n";
        }
    }

    // ----- indexdb meta keys (last_indexed, incomplete, walked_complete) -----
    if (fs::exists(indexDir)) {
        std::cout << "\n  indexdb meta keys:\n";
        auto meta = readIndexdbMeta(indexDir);
        if (!meta.ok) {
            std::cout << "    FAIL reading: " << meta.err << "\n";
            ++failed;
        } else {
            if (meta.hasLastIndexed) {
                std::cout << "    __last_indexed__   : " << meta.lastIndexed;
                if (chainHead != 0) {
                    if (meta.lastIndexed == chainHead)
                        std::cout << "  OK (matches chain head)";
                    else if (meta.lastIndexed <= chainHead && meta.lastIndexed + 5 >= chainHead)
                        std::cout << "  OK (within 5 of chain head=" << chainHead << ")";
                    else if (meta.lastIndexed > chainHead) {
                        std::cout << "  FAIL (ahead of chain head=" << chainHead
                                  << " by " << (meta.lastIndexed - chainHead) << ")";
                        ++failed;
                    } else
                        std::cout << "  WARN (behind chain head=" << chainHead
                                  << " by " << (chainHead - meta.lastIndexed) << ")";
                }
                std::cout << "\n";
            } else {
                std::cout << "    __last_indexed__   : MISSING (FAIL)\n";
                ++failed;
            }
            std::cout << "    __incomplete__     : "
                      << (meta.hasIncomplete ? std::to_string(int(meta.incomplete)) : std::string("MISSING"));
            if (meta.hasIncomplete && meta.incomplete != 0) {
                std::cout << "  FAIL (index marked incomplete)";
                ++failed;
            } else if (meta.hasIncomplete) std::cout << "  OK";
            std::cout << "\n";
            std::cout << "    __walked_complete__: "
                      << (meta.hasWalkedComplete ? std::to_string(int(meta.walkedComplete)) : std::string("MISSING"));
            if (meta.hasWalkedComplete && meta.walkedComplete != 1)
                std::cout << "  WARN (walk not flagged complete)";
            else if (meta.hasWalkedComplete) std::cout << "  OK";
            std::cout << "\n";
        }
    }

    // ----- cross-DB checks against chain (re-open chain DB read-only) -----
    if (chainHead != 0) {
        std::cout << "\n  Cross-DB checks (re-opening chain " << backend << " read-only)...\n";
        auto db = cs::chain_integrity::open_db(backend, (dataDir / "db").string());
        if (!db) {
            std::cout << "    FAIL: could not reopen chain DB for cross-checks\n";
            ++failed;
        } else {
            if (seqStats.ok && !seqStats.samples.empty()) {
                std::cout << "\n  seqdb -> chain (" << seqStats.samples.size() << " samples):\n";
                size_t m = 0, mm = 0, miss = 0;
                crossCheckSeqdbVsChain(*db, seqStats, m, mm, miss, 5);
                std::cout << "    matched=" << m << "  mismatched=" << mm
                          << "  missing_in_chain=" << miss
                          << ((mm || miss) ? "  FAIL\n" : "  OK\n");
                if (mm || miss) ++failed;
            }
            if (fs::exists(indexDir)) {
                std::cout << "\n  indexdb content spot-check (50 non-empty blocks):\n";
                size_t checked = 0, present = 0, absent = 0;
                crossCheckIndexdbAgainstChain(indexDir, *db, chainHead, 50,
                                              checked, present, absent);
                std::cout << "    checked=" << checked << "  present=" << present
                          << "  absent=" << absent
                          << (absent ? "  FAIL\n" : "  OK\n");
                if (absent) ++failed;
            }
        }
    }

    std::cout << "\nPhase 2: " << failed << " failure(s).\n";
    return failed > 0 ? 1 : 0;
}

// ===== Phase 3: db/ chain integrity =========================================

int auditChain(const fs::path& dataDir, const std::string& backend, cs::Sequence& chainHead) {
    std::cout << "\n========== PHASE 3: db/ chain integrity ==========\n";
    chainHead = 0;
    fs::path dbDir = dataDir / "db";
    if (!fs::exists(dbDir) || !fs::is_directory(dbDir)) {
        std::cout << "SKIP: " << dbDir.string() << " not a directory\n";
        return 0;
    }

    auto db = cs::chain_integrity::open_db(backend, dbDir.string());
    if (!db) {
        std::cout << "FAIL: could not open " << backend << " at " << dbDir.string() << "\n";
        return 1;
    }
    chainHead = cs::chain_integrity::db_top_sequence(*db);
    std::cout << "Backend: " << backend << "  Chain head sequence: " << chainHead << "\n";

    cs::chain_integrity::Options opts;
    opts.progress_every = 250000;
    opts.mismatch_print_limit = 50;
    opts.progress_log = &std::cout;
    auto report = cs::chain_integrity::verify_range(
        *db, 0, std::numeric_limits<cs::Sequence>::max(), opts);

    std::cout << "\n  scanned         : " << report.scanned << " blocks"
              << "  from=" << report.from << "  to=" << report.to << "\n"
              << "  prev-hash/seq   : " << report.mismatches << " mismatches\n"
              << "  gaps            : " << report.gaps << "\n"
              << "  decode failures : " << report.decode_failures << "\n"
              << "  missing pools   : " << report.missing << "\n";

    return report.ok() ? 0 : 1;
}

// ===== Main =================================================================

int run(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        std::cerr << "state_audit <data-dir> [--backend rocksdb|berkeleydb] [--log <path>]\n"
                     "  Phase 1 (qs/):     checkpoint structure\n"
                     "  Phase 2 (caches/): LMDB index consistency\n"
                     "  Phase 3 (db/):     chain prev_hash integrity walk\n"
                     "Default backend: rocksdb.\n"
                     "Default log path: ./state_audit.log (use --log /dev/null to disable).\n";
        std::_Exit(argc < 2 ? 2 : 0);
    }

    fs::path dataDir = argv[1];
    std::string backend = "rocksdb";
    std::string logPath = "state_audit.log";
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--backend") backend = argv[i + 1];
        else if (std::string(argv[i]) == "--log") logPath = argv[i + 1];
    }

    // Open log file and tee stdout + stderr into it.
    static std::ofstream logFile(logPath, std::ios::out | std::ios::trunc);
    static std::unique_ptr<TeeBuf> teeOut, teeErr;
    if (logFile.is_open()) {
        teeOut = std::make_unique<TeeBuf>(std::cout.rdbuf(), logFile.rdbuf());
        teeErr = std::make_unique<TeeBuf>(std::cerr.rdbuf(), logFile.rdbuf());
        std::cout.rdbuf(teeOut.get());
        std::cerr.rdbuf(teeErr.get());
        std::cout << "Log: writing to " << logPath << "\n";
    } else {
        std::cerr << "WARN: could not open log file " << logPath
                  << " — proceeding without log\n";
    }

    std::error_code ec;
    if (!fs::is_directory(dataDir, ec)) {
        std::cerr << "FAIL: " << dataDir.string() << " is not a directory";
        if (ec) std::cerr << " (" << ec.message() << ")";
        std::cerr << "\n";
        std::_Exit(2);
    }

    int p1 = auditQuickStart(dataDir);
    cs::Sequence chainHead = 0;
    int p3 = auditChain(dataDir, backend, chainHead);
    int p2 = auditCaches(dataDir, backend, chainHead);

    std::cout << "\n========== OVERALL RESULT ==========\n"
              << "  Phase 1 (qs/structure)        : " << (p1 ? "FAIL" : "OK") << "\n"
              << "  Phase 2 (caches LMDB)         : " << (p2 ? "FAIL" : "OK") << "\n"
              << "  Phase 3 (db/ chain integrity) : " << (p3 ? "FAIL" : "OK") << "\n";

    std::fflush(stdout);
    std::fflush(stderr);
    return (p1 | p2 | p3) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::bad_alloc&) {
        std::cerr << "FAIL: bad_alloc\n";
        std::fflush(stderr);
        std::_Exit(2);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        std::fflush(stderr);
        std::_Exit(2);
    } catch (...) {
        std::cerr << "FAIL: unknown exception\n";
        std::fflush(stderr);
        std::_Exit(2);
    }
}
