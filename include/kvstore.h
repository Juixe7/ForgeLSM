#ifndef FLSM_KVSTORE_H
#define FLSM_KVSTORE_H

#include "wal.h"
#include "vlog.h"
#include "memtable.h"
#include "sstable.h"
#include "manifest.h"

#include <memory>
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
    uint64_t get_calls = 0;
    uint64_t sst_considered = 0;
    uint64_t bloom_skips = 0;
    uint64_t sst_searches = 0;
    uint64_t vlog_reads = 0;

    void reset() {
        user_bytes_written = 0;
        storage_bytes_written = 0;
        get_calls = 0;
        sst_considered = 0;
        bloom_skips = 0;
        sst_searches = 0;
        vlog_reads = 0;
    }
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

    void put(const std::string& key, const std::string& value);
    void delete_key(const std::string& key);
    bool get(const std::string& key, std::string& out_value) const;

    size_t memtable_size() const;
    bool   wal_tainted() const;

    EngineMetrics& metrics() { return metrics_; }
    const EngineMetrics& metrics() const { return metrics_; }

    void add_storage_bytes(uint64_t bytes) { metrics_.storage_bytes_written += bytes; }
    void add_user_bytes(uint64_t bytes) { metrics_.user_bytes_written += bytes; }
    void subtract_user_bytes(uint64_t bytes) { metrics_.user_bytes_written -= bytes; }

    // ── Observability accessors (used by HttpServer) ──────────────
    size_t l0_count()            const { return l0_sstables_.size(); }
    size_t l1_count()            const { return l1_sstables_.size(); }
    size_t active_byte_size()    const { return active_ ? active_->byte_size() : 0; }
    size_t memtable_entries()    const { return active_ ? active_->size() : 0; }
    size_t flush_threshold_bytes() const { return FLUSH_THRESHOLD; }
    size_t l0_hard_limit()       const { return L0_HARD_LIMIT; }

    // Verification/admin hooks used by the local dashboard. These keep the
    // public app surface small while allowing the UI to prove storage behavior.
    void force_flush() { flush(); }
    void force_compaction() { compact_l0_to_l1(); }
    
    // Test helper to explicitly disable bloom filter and evaluate invariant equivalence
    void bypass_bloom(bool bypass) { disable_bloom_ = bypass; }

private:
    void     recover();
    void     load_sstables();
    void     scan_wal_files(std::vector<std::string>& paths, uint32_t& max_id) const;
    void     maybe_flush();
    void     flush();
    void     rotate_wal();
    void     compact_l0_to_l1();
    uint32_t next_sst_sequence() const;

    std::string manifest_path() const;

    std::string wal_path(uint32_t id) const;
    std::string vlog_path() const;
    std::string sst_path(uint32_t seq) const;

    std::string                  data_dir_;
    mutable EngineMetrics        metrics_;
    std::unique_ptr<WAL>         wal_;
    std::unique_ptr<VLog>        vlog_;
    std::unique_ptr<Memtable>    active_;
    std::unique_ptr<Memtable>    immutable_;
    Manifest                     manifest_;
    std::vector<SSTableReader>   l0_sstables_; // sorted newest-first
    std::vector<SSTableReader>   l1_sstables_; // non-overlapping
    uint32_t                     current_wal_id_ = 1;
    bool                         disable_bloom_ = false;

    static constexpr size_t FLUSH_THRESHOLD = 4u * 1024u * 1024u;  // 4 MiB
    static constexpr size_t L0_HARD_LIMIT   = 15;

    friend void run_compaction(KVStore* store);
    friend void run_vlog_gc(KVStore* store);
};

#endif // FLSM_KVSTORE_H
