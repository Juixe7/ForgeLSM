#include "compaction.h"
#include "kvstore.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

void run_compaction(KVStore* store) {
    auto& manifest = store->manifest_;
    if (manifest.l0_seqs.empty()) return;

    // 1. Snapshot inputs.
    std::vector<uint32_t> l0_inputs = manifest.l0_seqs;
    std::string global_min = "\xFF", global_max = "";

    auto get_l0_reader = [&](uint32_t seq) -> const SSTableReader* {
        for (const auto& r : store->l0_sstables_) {
            if (r.sequence() == seq) return &r;
        }
        return nullptr;
    };

    for (uint32_t seq : l0_inputs) {
        auto r = get_l0_reader(seq);
        if (!r) continue;
        if (r->min_key() < global_min) global_min = r->min_key();
        if (r->max_key() > global_max) global_max = r->max_key();
    }

    // 2. Find overlapping L1 files.
    std::vector<uint32_t> l1_inputs;
    std::vector<uint32_t> l1_retained;

    auto get_l1_reader = [&](uint32_t seq) -> const SSTableReader* {
        for (const auto& r : store->l1_sstables_) {
            if (r.sequence() == seq) return &r;
        }
        return nullptr;
    };

    for (uint32_t seq : manifest.l1_seqs) {
        auto r = get_l1_reader(seq);
        if (!r) continue;
        // Overlap detection via key range intersection.
        if (r->overlaps(global_min, global_max)) {
            l1_inputs.push_back(seq);
        } else {
            l1_retained.push_back(seq);
        }
    }

    // 3. Collect keys from input L1 files (for safe tombstone eviction).
    std::set<std::string> l1_keys;
    for (uint32_t seq : l1_inputs) {
        auto r = get_l1_reader(seq);
        for (const auto& e : r->entries()) {
            l1_keys.insert(e.key);
        }
    }

    // 4. K-Way merge (Duplicate Resolution: newest version wins).
    // COMPACTION ORDERING (STRICT PRIORITY):
    // std::map::insert ignores duplicates. By inserting sources in strictly newest-to-oldest order
    // (Newest L0 -> Oldest L0 -> L1), we naturally guarantee that only the newest sequence 
    // of any given key is retained. Older overlapping sequences are explicitly discarded.
    std::map<std::string, VLogPointer> merged;

    // Precedence 1: Newest L0 to Oldest L0.
    // L0 files are appended normally, so reverse order = newest first.
    for (auto it = l0_inputs.rbegin(); it != l0_inputs.rend(); ++it) {
        auto r = get_l0_reader(*it);
        if (!r) continue;
        for (const auto& e : r->entries()) {
            merged.insert({e.key, e.pointer}); // insert only succeeds if key not already present
        }
    }

    // Precedence 2: L1 inputs.
    for (uint32_t seq : l1_inputs) {
        auto r = get_l1_reader(seq);
        if (!r) continue;
        for (const auto& e : r->entries()) {
            merged.insert({e.key, e.pointer});
        }
    }

    // 5. Filter tombstones according to safety rules.
    for (auto it = merged.begin(); it != merged.end(); ) {
        if (is_tombstone(it->second)) {
            // ONLY drop tombstone if key does NOT exist in input L1 files.
            if (l1_keys.find(it->first) == l1_keys.end()) {
                it = merged.erase(it);
                continue;
            }
        }
        ++it;
    }

    // 6. Write new L1 SSTables (chunked by threshold).
    std::vector<uint32_t> new_l1_seqs;
    std::map<std::string, VLogPointer> chunk;
    size_t chunk_size = 0;

    auto flush_chunk = [&]() {
        if (chunk.empty()) return;
        uint32_t seq = store->next_sst_sequence();
        std::string path = store->sst_path(seq);
        if (!SSTableWriter::write(path, chunk)) {
            throw std::runtime_error("[Compaction] Failed to write new L1 SSTable");
        }
        store->add_storage_bytes(24); // Footer approx byte cost for the new L1 chunk
        new_l1_seqs.push_back(seq);
        
        // Emulate KVStore next_sst_sequence advancement for multi-chunk
        // Since next_sst_sequence derives from filesystem, the creation of the file
        // on disk automatically updates the sequence logically! 
        // But to be absolutely safe, let's keep chunking logic independent.

        chunk.clear();
        chunk_size = 0;
    };

    for (const auto& [k, v] : merged) {
        chunk[k] = v;
        chunk_size += k.size() + 20; // key + VLogPointer
        store->add_storage_bytes(k.size() + 20); // Metric tracking
        if (chunk_size >= KVStore::FLUSH_THRESHOLD) flush_chunk();
    }
    flush_chunk();

    // 7. Atomic Manifest Update (Visibility strictly tied to commit).
    manifest.version++;
    manifest.l0_seqs.clear(); // all L0 compacted
    manifest.l1_seqs = l1_retained;
    manifest.l1_seqs.insert(manifest.l1_seqs.end(), new_l1_seqs.begin(), new_l1_seqs.end());

    if (!manifest.commit(store->manifest_path())) {
        throw std::runtime_error("[Compaction] Manifest atomic rename failed");
    }

    std::cout << "[Compaction] Merged " << l0_inputs.size() << " L0 and " 
              << l1_inputs.size() << " L1 files into " 
              << new_l1_seqs.size() << " new L1 files.\n";

    // 8. Safely delete old compacted files from disk.
    std::error_code ec;
    for (uint32_t seq : l0_inputs) std::filesystem::remove(store->sst_path(seq), ec);
    for (uint32_t seq : l1_inputs) std::filesystem::remove(store->sst_path(seq), ec);

    // 9. Reload state so read path sees the new manifest state correctly.
    store->load_sstables();
}
