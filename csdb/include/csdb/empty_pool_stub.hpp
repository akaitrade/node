// tinycs empty-block stub format helpers.
// Layout v4 (tag 0xFC):
//   [tag:1][seq:8 LE][prev_hash:32][hash:32][realTrusted:8 LE][n_conf:1][conf:n*32][uf_size:4 LE][uf_blob:size]
// Layout v3 (tag 0xFD, legacy reads only):
//   [tag:1][seq:8 LE][prev_hash:32][hash:32][n_conf:1][conf:n*32][uf_size:4 LE][uf_blob:size]
// v3 lost numberTrusted_/realTrusted_ → block reward distribution undercounted.
// v4 preserves realTrusted_; numberTrusted_ is derived from n_conf on parse.
// Stubs replace the on-disk encoding of empty blocks (97%+ of CREDITS chain
// today). Public so tools (e.g. csdb_migrate's cross-verify) can detect and
// parse stubs without duplicating format constants.

#ifndef _CREDITS_CSDB_EMPTY_POOL_STUB_H_INCLUDED_
#define _CREDITS_CSDB_EMPTY_POOL_STUB_H_INCLUDED_

#include <csdb/internal/types.hpp>
#include <csdb/pool.hpp>

namespace csdb {

// True iff `bytes` parses as a well-formed empty-pool stub.
bool is_empty_pool_stub(const cs::Bytes& bytes);

// Reconstruct a Pool from a stub. Returns an invalid Pool on malformed input.
// Reconstructed pool has empty transactions / signatures by design.
Pool parse_empty_pool_stub(const cs::Bytes& bytes);

// Build the on-disk stub for a (necessarily empty-tx) Pool.
cs::Bytes build_empty_pool_stub(const Pool& pool);

}  // namespace csdb

#endif  // _CREDITS_CSDB_EMPTY_POOL_STUB_H_INCLUDED_
