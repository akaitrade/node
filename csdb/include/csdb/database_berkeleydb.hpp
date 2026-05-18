/**
 * @file database_berkeleydb.h
 * @author Evgeny Zalivochkin
 */

#ifndef _CREDITS_CSDB_DATABASE_BERKELEY_H_INCLUDED_
#define _CREDITS_CSDB_DATABASE_BERKELEY_H_INCLUDED_

#include <db_cxx.h>
#include <memory>
#include <thread>

#include <csdb/database.hpp>

namespace berkeleydb {
class DB;
class Status;
struct Options;
}  // namespace berkeleydb

namespace csdb {

class DatabaseBerkeleyDB : public Database {
public:
    DatabaseBerkeleyDB();
    ~DatabaseBerkeleyDB() override;

public:
    bool open(const std::string& path);

    // On-demand checkpoint + log archive; used by migrate at progress ticks.
    bool force_checkpoint();

    // Migration-only knob; must be called before open(). Bumps mpool cache and
    // moves the txn log into memory (no log files on disk). Sacrifices crash
    // recovery — only safe for one-shot, re-runnable workloads.
    void tune_for_bulk(size_t cache_bytes, uint32_t log_buf_bytes);

    // Direct write to blockchain.db with arbitrary RECNO key. Bypasses the
    // hash index. Used by migrate's experimental --bundle-size mode where
    // one record stores N concatenated blocks. dst becomes non-loadable.
    bool put_recno(uint32_t recno_key, const cs::Bytes& value);

private:
    bool is_open() const final;
    bool put(const cs::Bytes& key, uint32_t seq_no, const cs::Bytes& value) final;
    bool get(const cs::Bytes& key, cs::Bytes* value) final;
    bool get(const uint32_t seq_no, cs::Bytes* value) final;
    bool remove(const cs::Bytes&) final;
    bool seq_no(const cs::Bytes& key, uint32_t* value) final; // sequence from block hash
    bool write_batch(const ItemList&) final;
    IteratorPtr new_iterator() final;

    bool updateContractData(const cs::Bytes& key, const cs::Bytes& data) override;
    bool getContractData(const cs::Bytes& key, cs::Bytes& data) override;

    void logfile_routine();

private:
    class Iterator;

private:
    void set_last_error_from_berkeleydb(int status);

private:
    DbEnv env_;
    std::unique_ptr<Db> db_blocks_;
    std::unique_ptr<Db> db_seq_no_;
    std::unique_ptr<Db> db_contracts_;
    std::thread logfile_thread_;
    bool quit_ = false;
    bool bulk_mode_ = false;
};

}  // namespace csdb
#endif  // _CREDITS_CSDB_DATABASE_BERKELEYDB_H_INCLUDED_
