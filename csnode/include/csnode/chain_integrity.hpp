#pragma once
#include <cstddef>
#include <iosfwd>
#include <limits>
#include <memory>
#include <string>

#include <lib/system/common.hpp>

namespace csdb { class Database; class PoolHash; }

namespace cs::chain_integrity {

struct Options {
    size_t progress_every = 100000;
    size_t mismatch_print_limit = 50;
    std::ostream* progress_log = nullptr;
};

struct Report {
    cs::Sequence from = 0;
    cs::Sequence to = 0;
    size_t scanned = 0;
    size_t mismatches = 0;
    size_t gaps = 0;
    size_t decode_failures = 0;
    size_t missing = 0;
    bool ok() const { return mismatches == 0 && gaps == 0 && decode_failures == 0 && missing == 0; }
};

bool verify_at(csdb::Database& db, cs::Sequence seq, const cs::Bytes& expected_hash);

Report verify_range(csdb::Database& db,
                    cs::Sequence from = 0,
                    cs::Sequence to = std::numeric_limits<cs::Sequence>::max(),
                    const Options& opts = {});

cs::Sequence db_top_sequence(csdb::Database& db);

// open chain DB by backend name; returns nullptr if backend not compiled in or open fails
std::shared_ptr<csdb::Database> open_db(const std::string& backend, const std::string& path);

} // namespace cs::chain_integrity
