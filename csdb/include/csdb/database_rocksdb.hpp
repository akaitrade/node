// RocksDB-backed csdb::Database. CFs: blocks, seq_no, contracts.
// blocks keyed BE(seq+1). Selected via -DCSDB_BACKEND=rocksdb.

#ifndef _CREDITS_CSDB_DATABASE_ROCKSDB_H_INCLUDED_
#define _CREDITS_CSDB_DATABASE_ROCKSDB_H_INCLUDED_

#include <memory>
#include <string>

#include <csdb/database.hpp>

namespace rocksdb {
class DB;
class ColumnFamilyHandle;
class Status;
}  // namespace rocksdb

namespace csdb {

class DatabaseRocksDB : public Database {
public:
    DatabaseRocksDB();
    ~DatabaseRocksDB() override;

public:
    bool open(const std::string& path);

private:
    bool is_open() const final;
    bool put(const cs::Bytes& key, uint32_t seq_no, const cs::Bytes& value) final;
    bool put_batch(const std::vector<PendingWrite>& items) final;
    bool get(const cs::Bytes& key, cs::Bytes* value) final;
    bool get(const uint32_t seq_no, cs::Bytes* value) final;
    bool remove(const cs::Bytes&) final;
    bool seq_no(const cs::Bytes& key, uint32_t* value) final;
    bool write_batch(const ItemList&) final;
    IteratorPtr new_iterator() final;

    bool updateContractData(const cs::Bytes& key, const cs::Bytes& data) override;
    bool getContractData(const cs::Bytes& key, cs::Bytes& data) override;

private:
    class Iterator;
    void set_last_error_from_status(const rocksdb::Status& s);

private:
    std::unique_ptr<rocksdb::DB> db_;
    rocksdb::ColumnFamilyHandle* cf_blocks_ = nullptr;     // default CF
    rocksdb::ColumnFamilyHandle* cf_seq_no_ = nullptr;
    rocksdb::ColumnFamilyHandle* cf_contracts_ = nullptr;
};

}  // namespace csdb

#endif  // _CREDITS_CSDB_DATABASE_ROCKSDB_H_INCLUDED_
