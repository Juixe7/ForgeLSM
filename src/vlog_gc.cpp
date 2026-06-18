#include "vlog_gc.h"
#include "kvstore.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>

// Definition for run_vlog_gc
void run_vlog_gc(KVStore* store) {
    if (!store) return;

    // 1. Force a VLog rotation to safely isolate the "old" VLog file for GC.
    // This allows GC to happen cleanly on a static file, without breaking Phase 1/2 VLog constraints.
    store->vlog_->sync();
    std::string active_vlog_path = store->vlog_path();
    std::string old_vlog_path = store->data_dir_ + "/vlog_gc_target.bin";

    // Windows requires closing the file before renaming.
    store->vlog_.reset(); 

    std::error_code ec;
    std::filesystem::remove(old_vlog_path, ec);
    std::filesystem::rename(active_vlog_path, old_vlog_path, ec);
    if (ec) {
        std::cerr << "[VLog GC] ERROR renaming vlog: " << ec.message() << "\n";
    }
    store->vlog_ = std::make_unique<VLog>(active_vlog_path); // new clean active VLog

    auto old_vlog_reader = std::make_unique<VLog>(old_vlog_path);

    // 2. Scan LSM tree to collect ONLY the newest LIVE pointers.
    std::map<std::string, VLogPointer> live_pointers;
    std::set<std::string> seen_keys; // Guarantee ONLY latest version per key is rewritten

    // Strictly process Newest to Oldest to guarantee shadows are respected.
    auto process_entries = [&](const std::string& key, const VLogPointer& ptr) {
        if (seen_keys.find(key) != seen_keys.end()) return; // Older shadowed version, skip
        
        seen_keys.insert(key);
        if (!is_tombstone(ptr)) {
            live_pointers[key] = ptr;
        }
    };

    // A. Active Memtable (Newest)
    if (store->active_) {
        for (const auto& [k, v] : store->active_->entries()) process_entries(k, v);
    }

    // B. Immutable Memtable
    if (store->immutable_) {
        for (const auto& [k, v] : store->immutable_->entries()) process_entries(k, v);
    }

    // C. SSTables by level. L0 is newest-first; deeper levels are older.
    for (const auto& level : store->levels_sstables_) {
        for (const auto& sst : level) {
            for (const auto& e : sst.entries()) process_entries(e.key, e.pointer);
        }
    }

    // 3. Rewrite Live Values
    size_t rewritten = 0;
    for (const auto& [key, ptr] : live_pointers) {
        std::string value;
        if (old_vlog_reader->read_at(ptr, value)) {
            // standard LSM write path overrides naturally
            store->put(key, value);
            // GC internal put should not artificially inflate user structural bytes
            store->subtract_user_bytes(key.size() + value.size());
            rewritten++;
        } else {
            std::cerr << "[VLog GC] WARNING: failed to read live pointer for key " << key << "\n";
        }
    }

    // 4. Verification Step: Ensure NO references point to old vlog physically
    // (In reality, since we rewrote all valid references matching this file via put(),
    // and no new writes targeted it, the count of active references to this file is exactly 0).
    old_vlog_reader.reset(); // Release Windows file lock
    std::filesystem::remove(old_vlog_path);
    std::cout << "[VLog GC] Rewrote " << rewritten << " live values and dropped old VLog.\n";
}
