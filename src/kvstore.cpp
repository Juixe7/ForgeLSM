#include "kvstore.h"
#include "compaction.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// ── Path helpers ───────────────────────────────────────────────

std::string KVStore::wal_path(uint32_t id) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/wal_%06u.log", id);
    return data_dir_ + buf;
}

std::string KVStore::vlog_path() const { return data_dir_ + "/vlog.bin"; }

std::string KVStore::sst_path(uint32_t seq) const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/sst_%06u.sst", seq);
    return data_dir_ + buf;
}

std::string KVStore::manifest_path() const { return data_dir_ + "/MANIFEST"; }

uint32_t KVStore::next_sst_sequence() const {
    uint32_t max_seq = 0;
    if (!std::filesystem::exists(data_dir_)) return 1;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        auto name = entry.path().filename().string();
        if (name.size() > 4 && name.substr(0, 4) == "sst_" &&
            name.substr(name.size() - 4) == ".sst") {
            uint32_t seq = static_cast<uint32_t>(std::strtoul(name.c_str()+4, nullptr, 10));
            if (seq > max_seq) max_seq = seq;
        }
    }
    return max_seq + 1;
}

// Scan data_dir_ for wal_*.log files. Returns sorted paths and max id found.
void KVStore::scan_wal_files(std::vector<std::string>& paths, uint32_t& max_id) const {
    paths.clear();
    max_id = 0;
    if (!std::filesystem::exists(data_dir_)) return;

    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        auto name = entry.path().filename().string();
        if (name.size() > 4 && name.substr(0, 4) == "wal_" &&
            name.size() > 4 && name.substr(name.size() - 4) == ".log") {
            uint32_t id = static_cast<uint32_t>(
                std::strtoul(name.c_str() + 4, nullptr, 10));
            if (id > max_id) max_id = id;
            paths.push_back(entry.path().string());
        }
    }
    // Sort ascending by filename (lexicographic = numeric due to zero-padding).
    std::sort(paths.begin(), paths.end());
}

// ── Constructor ────────────────────────────────────────────────

KVStore::KVStore(const std::string& data_dir)
    : KVStore(data_dir, KVStoreOptions{}) {}

KVStore::KVStore(const std::string& data_dir, const KVStoreOptions& options)
    : data_dir_(data_dir), options_(options) {
    if (options_.trace_path.empty()) options_.trace_path = data_dir_ + "/engine_trace.log";
    std::filesystem::create_directories(data_dir_);
    if (options_.trace_enabled) {
        std::ofstream out(options_.trace_path, std::ios::app);
        out << "[trace] open store dir=" << data_dir_ << "\n";
    }
    recover();
}

// ── Write path ─────────────────────────────────────────────────

void KVStore::delete_key(const std::string& key) {
    maybe_flush();
    trace_event("delete:start", "key=" + key);
    if (!wal_->append_delete(key))
        throw std::runtime_error("[KVStore] WAL append_delete failed");
    trace_event("wal:append-delete", "key=" + key);
    if (!wal_->sync())
        throw std::runtime_error("[KVStore] WAL sync failed");
    trace_event("wal:sync", "delete key=" + key);

    VLogPointer ptr;
    ptr.length = 0;
    ptr.offset = std::numeric_limits<uint64_t>::max();
    ptr.file_id = current_wal_id_;
    active_->put(key, ptr);
    trace_event("memtable:tombstone", "key=" + key);
}

void KVStore::put(const std::string& key, const std::string& value) {
    maybe_flush();
    trace_event("put:start", "key=" + key + " value_bytes=" + std::to_string(value.size()));

    // Step 1: WAL append (full key + value).
    // Write Amp Metric additions
    metrics_.user_bytes_written += key.size() + value.size();
    metrics_.storage_bytes_written += 12 + key.size() + value.size(); // WAL struct overhead
    metrics_.storage_bytes_written += 4 + value.size();               // VLog overhead

    if (!wal_->append(key, value))
        throw std::runtime_error("[KVStore] WAL append failed");
    trace_event("wal:append", "key=" + key);

    // Step 2: WAL sync — durability boundary.
    if (!wal_->sync())
        throw std::runtime_error("[KVStore] WAL sync failed");
    trace_event("wal:sync", "key=" + key);

    // Step 3: VLog append — returns pointer.
    VLogPointer ptr;
    if (!vlog_->append(value, ptr))
        throw std::runtime_error("[KVStore] VLog append failed");
    trace_event("vlog:append", "key=" + key + " offset=" + std::to_string(ptr.offset) + " bytes=" + std::to_string(ptr.length));

    // Step 4: VLog sync — pointer validity boundary.
    if (!vlog_->sync())
        throw std::runtime_error("[KVStore] VLog sync failed — pointer NOT inserted");
    trace_event("vlog:sync", "key=" + key);

    // Step 5: Memtable put — only reached if all above succeeded.
    active_->put(key, ptr);
    trace_event("memtable:put", "key=" + key + " entries=" + std::to_string(active_->size()));
}

// ── Read path ──────────────────────────────────────────────────

bool KVStore::get(const std::string& key, std::string& out_value) const {
    metrics_.get_calls++;
    VLogPointer ptr;

    // 1. Active memtable.
    if (active_ && active_->get(key, ptr)) {
        if (is_tombstone(ptr)) return false;
        metrics_.vlog_reads++;
        return vlog_->read_at(ptr, out_value);
    }

    // 2. Immutable memtable (exists during flush).
    if (immutable_ && immutable_->get(key, ptr)) {
        if (is_tombstone(ptr)) return false;
        metrics_.vlog_reads++;
        return vlog_->read_at(ptr, out_value);
    }

    // 3. L0 SSTables — newest first.
    for (const auto& sst : l0_sstables_) {
        metrics_.sst_considered++;
        if (!disable_bloom_ && !sst.bloom().may_contain(key)) {
            metrics_.bloom_skips++;
            continue; // SKIP completely
        }
        
        metrics_.sst_searches++; // Only count actual binary search checks
        if (sst.get(key, ptr)) {
            if (is_tombstone(ptr)) return false;
            metrics_.vlog_reads++;
            return vlog_->read_at(ptr, out_value);
        }
    }

    // 4. L1 SSTables — binary search file boundaries.
    for (const auto& sst : l1_sstables_) {
        // Find the overlapping file:
        if (sst.overlaps(key, key)) {
            metrics_.sst_considered++;
            if (!disable_bloom_ && !sst.bloom().may_contain(key)) {
                metrics_.bloom_skips++;
                continue; // SKIP completely
            }
            
            metrics_.sst_searches++;
            if (sst.get(key, ptr)) {
                if (is_tombstone(ptr)) return false;
                metrics_.vlog_reads++;
                return vlog_->read_at(ptr, out_value);
            }
        }
    }

    // 5. L2 SSTables — colder compacted level.
    for (const auto& sst : l2_sstables_) {
        if (sst.overlaps(key, key)) {
            metrics_.sst_considered++;
            if (!disable_bloom_ && !sst.bloom().may_contain(key)) {
                metrics_.bloom_skips++;
                continue;
            }

            metrics_.sst_searches++;
            if (sst.get(key, ptr)) {
                if (is_tombstone(ptr)) return false;
                metrics_.vlog_reads++;
                return vlog_->read_at(ptr, out_value);
            }
        }
    }

    return false;
}

// ── Flush ──────────────────────────────────────────────────────

void KVStore::maybe_flush() {
    // BACKPRESSURE SAFETY CHECK (I21):
    // Put() calls maybe_flush(), which directly and synchronously executes compaction 
    // when L0 threshold is exceeded. Because Phase 3 is explicitly single-threaded 
    // and holds no cross-thread locks, there is NO deadlock risk.
    if (l1_sstables_.size() > options_.l1_hard_limit) {
        compact_l1_to_l2();
    }
    if (l0_sstables_.size() > options_.l0_hard_limit) {
        compact_l0_to_l1();
    }
    if (!active_ || active_->byte_size() < options_.flush_threshold) return;
    flush();
}

void KVStore::flush() {
    if (!active_ || active_->size() == 0) return;

    // 1. Freeze active → immutable.
    trace_event("flush:start", "entries=" + std::to_string(active_->size()) + " bytes=" + std::to_string(active_->byte_size()));
    immutable_ = std::move(active_);
    active_ = std::make_unique<Memtable>();

    // 2. Write SSTable from immutable memtable.
    uint32_t seq = next_sst_sequence();
    std::string path = sst_path(seq);

    // Track write amplification for SST flush
    size_t sst_est = 24; // footer approx
    for (const auto& [k,v] : immutable_->entries()) sst_est += 20 + k.size();
    add_storage_bytes(sst_est);

    if (!SSTableWriter::write(path, immutable_->entries()))
        throw std::runtime_error("[KVStore] SSTable flush failed");
    trace_event("sstable:write", path);

    // 3. Update manifest atomically. New SST forms L0 and is visible AFTER commit.
    manifest_.version++;
    manifest_.l0_seqs.push_back(seq);
    if (!manifest_.commit(manifest_path()))
        throw std::runtime_error("[KVStore] Manifest commit failed during flush");
    trace_event("manifest:commit", "L0 add seq=" + std::to_string(seq));

    // 4. Load the new SSTable.
    SSTableReader reader;
    if (!reader.load(path))
        throw std::runtime_error("[KVStore] Failed to load flushed SSTable");

    l0_sstables_.insert(l0_sstables_.begin(), std::move(reader));

    // 5. WAL rotation (crash-safe: create-before-delete, I19).
    rotate_wal();

    // 6. Discard immutable memtable.
    immutable_.reset();

    std::cout << "[KVStore] Flushed SSTable sst_"
              << std::string(6 - std::to_string(seq).size(), '0') + std::to_string(seq)
              << "\n";
    trace_event("flush:done", "seq=" + std::to_string(seq));
}

// ── WAL rotation (crash-safe, I19) ─────────────────────────────
//
// Sequence:
//   1. Create NEW WAL at wal_{id+1}.log → fsync
//   2. Switch KVStore to new WAL (close old fd)
//   3. Delete old WAL at wal_{id}.log
//
// Old WAL is NEVER deleted before new WAL is durable.
// If crash between steps 1 and 3: both WAL files exist on disk.
// Recovery replays all WAL files in order — duplicates resolved by I8.

void KVStore::rotate_wal() {
    uint32_t old_id = current_wal_id_;
    uint32_t new_id = old_id + 1;
    std::string old_wp = wal_path(old_id);
    std::string new_wp = wal_path(new_id);

    // 1. Create new WAL, fsync (durable BEFORE we touch old).
    auto new_wal = std::make_unique<WAL>(new_wp);
    new_wal->sync();

    // 2. Switch: old WAL destructor closes its fd.
    wal_ = std::move(new_wal);
    current_wal_id_ = new_id;

    // 3. NOW safe to delete old WAL (new WAL is durable).
    std::error_code ec;
    std::filesystem::remove(old_wp, ec);
    if (ec) {
        std::cerr << "[KVStore] WARNING: could not delete old WAL " << old_wp << ": " << ec.message() << "\n";
    }
}

// ── Recovery ───────────────────────────────────────────────────

void KVStore::recover() {
    // Load existing SSTables (validate each).
    load_sstables();

    // Scan for WAL files.
    std::vector<std::string> wal_files;
    uint32_t max_wal_id = 0;
    scan_wal_files(wal_files, max_wal_id);

    // VLog handling:
    //   If SSTables exist → keep vlog (SST pointers reference it).
    //   If no SSTables    → safe to recreate vlog from WAL.
    auto vp = vlog_path();
    if (l0_sstables_.empty() && l1_sstables_.empty() && l2_sstables_.empty()) {
        std::error_code ec;
        std::filesystem::remove(vp, ec);
    }

    vlog_ = std::make_unique<VLog>(vp);

    // Replay ALL WAL files in order (oldest → newest).
    active_ = std::make_unique<Memtable>();
    size_t total_entries = 0;
    bool   any_tainted = false;

    for (const auto& wf : wal_files) {
        WAL temp_wal(wf);
        auto result = temp_wal.replay();
        any_tainted = any_tainted || result.tainted;

        for (const auto& e : result.entries) {
            if (e.is_tombstone) {
                VLogPointer ptr;
                ptr.length = 0;
                ptr.offset = std::numeric_limits<uint64_t>::max();
                ptr.file_id = 0;
                active_->put(e.key, ptr);
                continue;
            }

            VLogPointer ptr;
            if (!vlog_->append(e.value, ptr)) {
                std::cerr << "[KVStore] ERROR: vlog append failed during recovery\n";
                continue;
            }
            active_->put(e.key, ptr);
        }
        total_entries += result.entries.size();
    }
    vlog_->sync();

    // Set current WAL id and open the active WAL.
    current_wal_id_ = (max_wal_id > 0) ? max_wal_id : 1;

    // If WAL files existed, the newest is already the active one.
    // If no WAL files existed, create the first one.
    wal_ = std::make_unique<WAL>(wal_path(current_wal_id_));

    std::cout << "[KVStore] Recovered " << total_entries << " entries from "
              << wal_files.size() << " WAL(s)";
    if (!l0_sstables_.empty() || !l1_sstables_.empty() || !l2_sstables_.empty())
        std::cout << ", loaded " << (l0_sstables_.size() + l1_sstables_.size() + l2_sstables_.size()) << " SSTables";
    if (any_tainted)
        std::cout << " (WAL TAINTED)";
    std::cout << "\n";
}

// ── SSTable loading ────────────────────────────────────────────

void KVStore::load_sstables() {
    l0_sstables_.clear();
    l1_sstables_.clear();
    l2_sstables_.clear();
    if (!std::filesystem::exists(data_dir_)) return;

    if (!manifest_.load(manifest_path())) {
        manifest_.version = 0;
        return; // No manifest yet
    }

    // Load L0 files. Reverse order since l0_seqs are appended in flush (oldest first).
    // The vector l0_sstables_ must be newest-first for correct reading.
    for (auto it = manifest_.l0_seqs.rbegin(); it != manifest_.l0_seqs.rend(); ++it) {
        SSTableReader reader;
        if (reader.load(sst_path(*it))) {
            l0_sstables_.push_back(std::move(reader));
        } else {
            std::cerr << "[KVStore] WARNING: Manifest invalid L0 SSTable " << *it << "\n";
        }
    }

    // Load L1 files.
    for (uint32_t seq : manifest_.l1_seqs) {
        SSTableReader reader;
        if (reader.load(sst_path(seq))) {
            l1_sstables_.push_back(std::move(reader));
        } else {
            std::cerr << "[KVStore] WARNING: Manifest invalid L1 SSTable " << seq << "\n";
        }
    }

    // Load L2 files.
    for (uint32_t seq : manifest_.l2_seqs) {
        SSTableReader reader;
        if (reader.load(sst_path(seq))) {
            l2_sstables_.push_back(std::move(reader));
        } else {
            std::cerr << "[KVStore] WARNING: Manifest invalid L2 SSTable " << seq << "\n";
        }
    }
}

void KVStore::compact_l0_to_l1() {
    run_compaction(this);
}

void KVStore::compact_l1_to_l2() {
    run_l1_to_l2_compaction(this);
}

// ── Diagnostics ────────────────────────────────────────────────

size_t KVStore::memtable_size() const {
    size_t n = active_ ? active_->size() : 0;
    if (immutable_) n += immutable_->size();
    return n;
}

bool KVStore::wal_tainted() const {
    return wal_ && wal_->is_tainted();
}

StorageSummary KVStore::storage_summary() const {
    std::map<std::string, VLogPointer> newest;

    auto add_entries = [&](const std::map<std::string, VLogPointer>& entries) {
        for (const auto& [key, ptr] : entries) {
            newest.insert({key, ptr});
        }
    };
    auto add_sstable = [&](const SSTableReader& sst) {
        for (const auto& entry : sst.entries()) {
            newest.insert({entry.key, entry.pointer});
        }
    };

    if (active_) add_entries(active_->entries());
    if (immutable_) add_entries(immutable_->entries());

    for (const auto& sst : l0_sstables_) add_sstable(sst);
    for (const auto& sst : l1_sstables_) add_sstable(sst);
    for (const auto& sst : l2_sstables_) add_sstable(sst);

    StorageSummary summary;
    for (const auto& [key, ptr] : newest) {
        if (is_tombstone(ptr)) {
            summary.tombstones++;
            continue;
        }
        summary.live_keys++;
        summary.live_logical_bytes += static_cast<uint64_t>(key.size()) + ptr.length;
    }
    return summary;
}

void KVStore::trace_event(const std::string& event, const std::string& detail) const {
    if (!options_.trace_enabled) return;
    std::ofstream out(options_.trace_path, std::ios::app);
    if (!out) return;
    out << event << " | " << detail << "\n";
}
