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

std::string KVStore::stats_path() const { return data_dir_ + "/STATS"; }

std::string KVStore::gc_state_path() const { return data_dir_ + "/GC_STATE"; }

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
    load_stats();
    recover();
}

void KVStore::load_stats() {
    durable_metrics_.reset();
    std::ifstream in(stats_path());
    if (!in) return;

    std::string key;
    uint64_t value = 0;
    while (in >> key >> value) {
        if (key == "user_bytes_written") durable_metrics_.user_bytes_written = value;
        else if (key == "storage_bytes_written") durable_metrics_.storage_bytes_written = value;
        else if (key == "put_calls") durable_metrics_.put_calls = value;
        else if (key == "delete_calls") durable_metrics_.delete_calls = value;
    }
}

void KVStore::persist_stats() const {
    std::string tmp = stats_path() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << "user_bytes_written " << durable_metrics_.user_bytes_written << "\n";
        out << "storage_bytes_written " << durable_metrics_.storage_bytes_written << "\n";
        out << "put_calls " << durable_metrics_.put_calls << "\n";
        out << "delete_calls " << durable_metrics_.delete_calls << "\n";
        out.flush();
    }

    std::error_code ec;
    std::filesystem::remove(stats_path(), ec);
    ec.clear();
    std::filesystem::rename(tmp, stats_path(), ec);
}

void KVStore::record_put_metrics(uint64_t user_bytes, uint64_t storage_bytes) {
    metrics_.put_calls++;
    metrics_.user_bytes_written += user_bytes;
    metrics_.storage_bytes_written += storage_bytes;
    durable_metrics_.put_calls++;
    durable_metrics_.user_bytes_written += user_bytes;
    durable_metrics_.storage_bytes_written += storage_bytes;
    persist_stats();
}

void KVStore::record_delete_metrics(uint64_t user_bytes, uint64_t storage_bytes) {
    metrics_.delete_calls++;
    metrics_.user_bytes_written += user_bytes;
    metrics_.storage_bytes_written += storage_bytes;
    durable_metrics_.delete_calls++;
    durable_metrics_.user_bytes_written += user_bytes;
    durable_metrics_.storage_bytes_written += storage_bytes;
    persist_stats();
}

void KVStore::add_storage_bytes(uint64_t bytes) {
    metrics_.storage_bytes_written += bytes;
    durable_metrics_.storage_bytes_written += bytes;
    persist_stats();
}

void KVStore::add_user_bytes(uint64_t bytes) {
    metrics_.user_bytes_written += bytes;
    durable_metrics_.user_bytes_written += bytes;
    persist_stats();
}

void KVStore::subtract_user_bytes(uint64_t bytes) {
    metrics_.user_bytes_written = (bytes > metrics_.user_bytes_written) ? 0 : metrics_.user_bytes_written - bytes;
    durable_metrics_.user_bytes_written = (bytes > durable_metrics_.user_bytes_written) ? 0 : durable_metrics_.user_bytes_written - bytes;
    persist_stats();
}

void KVStore::reset_store() {
    wal_.reset();
    vlog_.reset();
    active_.reset();
    immutable_.reset();
    levels_sstables_.clear();
    manifest_ = Manifest{};
    current_wal_id_ = 1;
    metrics_.reset();
    durable_metrics_.reset();

    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
    std::filesystem::create_directories(data_dir_);
    persist_stats();
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
    record_delete_metrics(static_cast<uint64_t>(key.size()),
                          static_cast<uint64_t>(12 + key.size()));
    trace_event("memtable:tombstone", "key=" + key);
}

void KVStore::put(const std::string& key, const std::string& value) {
    maybe_flush();
    trace_event("put:start", "key=" + key + " value_bytes=" + std::to_string(value.size()));

    // Step 1: WAL append (full key + value).
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
    record_put_metrics(static_cast<uint64_t>(key.size() + value.size()),
                       static_cast<uint64_t>(12 + key.size() + value.size() + 4 + value.size()));
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

    // 3. SSTables — L0 newest-first, then older levels.
    for (size_t level = 0; level < levels_sstables_.size(); ++level) {
        for (const auto& sst : levels_sstables_[level]) {
            if (level > 0 && !sst.overlaps(key, key)) continue;

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
    bool compacted = true;
    while (compacted) {
        compacted = false;
        for (size_t level = 1; level < levels_sstables_.size(); ++level) {
            if (levels_sstables_[level].size() > level_hard_limit(level)) {
                compact_level(level);
                compacted = true;
                break;
            }
        }
    }
    if (level_count(0) > options_.l0_hard_limit) {
        compact_level(0);
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
    manifest_.ensure_level(0).push_back(seq);
    if (!manifest_.commit(manifest_path()))
        throw std::runtime_error("[KVStore] Manifest commit failed during flush");
    trace_event("manifest:commit", "L0 add seq=" + std::to_string(seq));

    // 4. Load the new SSTable.
    SSTableReader reader;
    if (!reader.load(path))
        throw std::runtime_error("[KVStore] Failed to load flushed SSTable");

    if (levels_sstables_.empty()) levels_sstables_.resize(1);
    levels_sstables_[0].insert(levels_sstables_[0].begin(), std::move(reader));

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
    recover_incomplete_gc();

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
    bool has_sstables = false;
    for (const auto& level : levels_sstables_) {
        if (!level.empty()) {
            has_sstables = true;
            break;
        }
    }
    if (!has_sstables) {
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
    size_t loaded_sstables = 0;
    for (const auto& level : levels_sstables_) loaded_sstables += level.size();
    if (loaded_sstables > 0)
        std::cout << ", loaded " << loaded_sstables << " SSTables";
    if (any_tainted)
        std::cout << " (WAL TAINTED)";
    std::cout << "\n";
}

void KVStore::recover_incomplete_gc() {
    const std::filesystem::path state_path(gc_state_path());
    if (!std::filesystem::exists(state_path)) return;

    std::string phase;
    {
        std::ifstream in(state_path);
        std::string key;
        while (in >> key) {
            if (key == "phase") in >> phase;
        }
    }

    std::filesystem::path active_vlog(vlog_path());
    std::filesystem::path old_vlog(data_dir_ + "/vlog_gc_target.bin");
    std::error_code ec;

    if (phase == "rewrite_complete") {
        std::filesystem::remove(old_vlog, ec);
        ec.clear();
        std::filesystem::remove(state_path, ec);
        std::cout << "[VLog GC] Recovery finalized completed GC state.\n";
        return;
    }

    // Any earlier phase is rolled back by restoring the isolated old VLog.
    // Rewritten GC puts, if any, are still safely replayed from WAL.
    if (std::filesystem::exists(old_vlog, ec)) {
        std::filesystem::remove(active_vlog, ec);
        ec.clear();
        std::filesystem::rename(old_vlog, active_vlog, ec);
        if (ec) {
            std::cerr << "[VLog GC] WARNING: failed to restore old VLog during recovery: "
                      << ec.message() << "\n";
        } else {
            std::cout << "[VLog GC] Recovery rolled back incomplete GC state.\n";
        }
    }

    ec.clear();
    std::filesystem::remove(state_path, ec);
}

// ── SSTable loading ────────────────────────────────────────────

void KVStore::load_sstables() {
    levels_sstables_.clear();
    if (!std::filesystem::exists(data_dir_)) return;

    if (!manifest_.load(manifest_path())) {
        manifest_.version = 0;
        return; // No manifest yet
    }

    levels_sstables_.resize(manifest_.level_count());
    for (size_t level = 0; level < manifest_.level_count(); ++level) {
        const auto& seqs = manifest_.level(level);
        if (level == 0) {
            // L0 is append-ordered in the manifest, but must be searched newest-first.
            for (auto it = seqs.rbegin(); it != seqs.rend(); ++it) {
                SSTableReader reader;
                if (reader.load(sst_path(*it))) {
                    levels_sstables_[level].push_back(std::move(reader));
                } else {
                    std::cerr << "[KVStore] WARNING: Manifest invalid L" << level << " SSTable " << *it << "\n";
                }
            }
        } else {
            for (uint32_t seq : seqs) {
                SSTableReader reader;
                if (reader.load(sst_path(seq))) {
                    levels_sstables_[level].push_back(std::move(reader));
                } else {
                    std::cerr << "[KVStore] WARNING: Manifest invalid L" << level << " SSTable " << seq << "\n";
                }
            }
        }
    }
}

void KVStore::compact_l0_to_l1() {
    compact_level(0);
}

void KVStore::compact_l1_to_l2() {
    compact_level(1);
}

void KVStore::compact_level(size_t level) {
    run_level_compaction(this, level);
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

size_t KVStore::level_hard_limit(size_t level) const {
    if (level == 0) return options_.l0_hard_limit;
    size_t limit = options_.l1_hard_limit;
    size_t multiplier = options_.level_size_multiplier == 0 ? 1 : options_.level_size_multiplier;
    for (size_t i = 1; i < level; ++i) {
        if (limit > std::numeric_limits<size_t>::max() / multiplier) {
            return std::numeric_limits<size_t>::max();
        }
        limit *= multiplier;
    }
    return limit;
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

    for (const auto& level : levels_sstables_) {
        for (const auto& sst : level) add_sstable(sst);
    }

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

void KVStore::set_trace_enabled(bool enabled) {
    options_.trace_enabled = enabled;
    if (options_.trace_path.empty()) options_.trace_path = data_dir_ + "/engine_trace.log";
    if (enabled) {
        std::ofstream out(options_.trace_path, std::ios::app);
        if (out) out << "[trace] enabled store dir=" << data_dir_ << "\n";
    }
}
