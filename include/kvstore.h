#ifndef FLSM_KVSTORE_H
#define FLSM_KVSTORE_H

#include "wal.h"
#include "vlog.h"
#include "memtable.h"
#include "sstable.h"
#include "manifest.h"

#include <memory>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <limits>

// Tombstone helper
inline bool is_tombstone(const VLogPointer& ptr) {
    return ptr.length == 0 && ptr.offset == std::numeric_limits<uint64_t>::max();
}

struct EngineMetrics {
    uint64_t user_bytes_written = 0;
    uint64_t storage_bytes_written = 0;
    uint64_t put_calls = 0;
    uint64_t delete_calls = 0;
    uint64_t get_calls = 0;
    uint64_t sst_considered = 0;
    uint64_t bloom_skips = 0;
    uint64_t sst_searches = 0;
    uint64_t vlog_reads = 0;

    void reset() {
        user_bytes_written = 0;
        storage_bytes_written = 0;
        put_calls = 0;
        delete_calls = 0;
        get_calls = 0;
        sst_considered = 0;
        bloom_skips = 0;
        sst_searches = 0;
        vlog_reads = 0;
    }
};

struct KVStoreOptions {
    size_t flush_threshold = 4u * 1024u * 1024u;
    size_t l0_hard_limit = 15;
    size_t l1_hard_limit = 12;
    size_t level_size_multiplier = 10;
    bool trace_enabled = false;
    std::string trace_path;
};

struct StorageSummary {
    uint64_t live_keys = 0;
    uint64_t tombstones = 0;
    uint64_t live_logical_bytes = 0;
};

// KVStore — engine core (Phase 2).
//
// Write path (strict order):
//   1. WAL.append(key, value)    — full record
//   2. WAL.sync()                — durability boundary
//   3. VLog.append(value)        — returns pointer
//   4. VLog.sync()               — pointer validity boundary
//   5. Memtable.put(key, pointer)— only if 1–4 succeed
//
// Read path:
//   active memtable → immutable memtable → SSTables (newest-first) → VLog read
//
// WAL files: wal_NNNNNN.log (monotonically increasing).
// Rotation: create new WAL → fsync → switch → delete old (I19 safe).
class KVStore {
public:
    explicit KVStore(const std::string& data_dir);
    KVStore(const std::string& data_dir, const KVStoreOptions& options);

    void put(const std::string& key, const std::string& value);
    void delete_key(const std::string& key);
    bool get(const std::string& key, std::string& out_value) const;
    void reset_store();

    size_t memtable_size() const;
    bool   wal_tainted() const;

    EngineMetrics& metrics() { return metrics_; }
    const EngineMetrics& metrics() const { return metrics_; }
    const EngineMetrics& durable_metrics() const { return durable_metrics_; }

    void add_storage_bytes(uint64_t bytes);
    void add_user_bytes(uint64_t bytes);
    void subtract_user_bytes(uint64_t bytes);

    // ── Observability accessors (used by HttpServer) ──────────────
    size_t level_count(size_t level) const {
        return level < levels_sstables_.size() ? levels_sstables_[level].size() : 0;
    }
    size_t max_level() const { return levels_sstables_.empty() ? 0 : levels_sstables_.size() - 1; }
    size_t l0_count()            const { return level_count(0); }
    size_t l1_count()            const { return level_count(1); }
    size_t l2_count()            const { return level_count(2); }
    size_t active_byte_size()    const { return active_ ? active_->byte_size() : 0; }
    size_t memtable_entries()    const { return active_ ? active_->size() : 0; }
    size_t flush_threshold_bytes() const { return options_.flush_threshold; }
    size_t l0_hard_limit()       const { return options_.l0_hard_limit; }
    size_t l1_hard_limit()       const { return options_.l1_hard_limit; }
    size_t level_hard_limit(size_t level) const;
    StorageSummary storage_summary() const;

    // Verification/admin hooks used by the local dashboard. These keep the
    // public app surface small while allowing the UI to prove storage behavior.
    void force_flush() { flush(); }
    void force_compaction() { compact_l0_to_l1(); }
    void force_l1_compaction() { compact_l1_to_l2(); }
    void force_level_compaction(size_t level) { compact_level(level); }
    void trace_event(const std::string& event, const std::string& detail) const;
    void set_trace_enabled(bool enabled);
    bool trace_enabled() const { return options_.trace_enabled; }
    std::string trace_path() const { return options_.trace_path; }
    
    // Test helper to explicitly disable bloom filter and evaluate invariant equivalence
    void bypass_bloom(bool bypass) { disable_bloom_ = bypass; }

private:
    void     recover();
    void     recover_incomplete_gc();
    void     load_sstables();
    void     scan_wal_files(std::vector<std::string>& paths, uint32_t& max_id) const;
    void     maybe_flush();
    void     flush();
    void     rotate_wal();
    void     compact_l0_to_l1();
    void     compact_l1_to_l2();
    void     compact_level(size_t level);
    uint32_t next_sst_sequence() const;
    void     load_stats();
    void     persist_stats() const;
    void     record_put_metrics(uint64_t user_bytes, uint64_t storage_bytes);
    void     record_delete_metrics(uint64_t user_bytes, uint64_t storage_bytes);

    std::string manifest_path() const;
    std::string stats_path() const;
    std::string gc_state_path() const;

    std::string wal_path(uint32_t id) const;
    std::string vlog_path() const;
    std::string sst_path(uint32_t seq) const;

    std::string                  data_dir_;
    mutable EngineMetrics        metrics_;
    EngineMetrics                durable_metrics_;
    std::unique_ptr<WAL>         wal_;
    std::unique_ptr<VLog>        vlog_;
    std::unique_ptr<Memtable>    active_;
    std::unique_ptr<Memtable>    immutable_;
    Manifest                     manifest_;
    std::vector<std::vector<SSTableReader>> levels_sstables_; // L0 newest-first, L1+ non-overlapping
    uint32_t                     current_wal_id_ = 1;
    bool                         disable_bloom_ = false;
    KVStoreOptions               options_;

    friend void run_compaction(KVStore* store);
    friend void run_l1_to_l2_compaction(KVStore* store);
    friend void run_level_compaction(KVStore* store, size_t source_level);
    friend void run_vlog_gc(KVStore* store);
};

#endif // FLSM_KVSTORE_H
