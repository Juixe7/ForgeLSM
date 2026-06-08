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

    // 3. K-Way merge (Duplicate Resolution: newest version wins).
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

    // Tombstones must be preserved when compacting into L1. Older values may
    // already live in L2, so dropping a tombstone here can resurrect deleted
    // keys after restart or after read path reaches colder levels.

    // 4. Write new L1 SSTables (chunked by threshold).
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
        if (chunk_size >= store->flush_threshold_bytes()) flush_chunk();
    }
    flush_chunk();

    // 5. Atomic Manifest Update (Visibility strictly tied to commit).
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

    // 6. Safely delete old compacted files from disk.
    std::error_code ec;
    for (uint32_t seq : l0_inputs) std::filesystem::remove(store->sst_path(seq), ec);
    for (uint32_t seq : l1_inputs) std::filesystem::remove(store->sst_path(seq), ec);

    // 7. Reload state so read path sees the new manifest state correctly.
    store->load_sstables();
}

void run_l1_to_l2_compaction(KVStore* store) {
    auto& manifest = store->manifest_;
    if (manifest.l1_seqs.empty()) return;

    std::vector<uint32_t> l1_inputs = manifest.l1_seqs;
    std::string global_min = "\xFF", global_max = "";

    auto get_l1_reader = [&](uint32_t seq) -> const SSTableReader* {
        for (const auto& r : store->l1_sstables_) {
            if (r.sequence() == seq) return &r;
        }
        return nullptr;
    };

    auto get_l2_reader = [&](uint32_t seq) -> const SSTableReader* {
        for (const auto& r : store->l2_sstables_) {
            if (r.sequence() == seq) return &r;
        }
        return nullptr;
    };

    for (uint32_t seq : l1_inputs) {
        auto r = get_l1_reader(seq);
        if (!r) continue;
        if (r->min_key() < global_min) global_min = r->min_key();
        if (r->max_key() > global_max) global_max = r->max_key();
    }

    std::vector<uint32_t> l2_inputs;
    std::vector<uint32_t> l2_retained;
    for (uint32_t seq : manifest.l2_seqs) {
        auto r = get_l2_reader(seq);
        if (!r) continue;
        if (r->overlaps(global_min, global_max)) {
            l2_inputs.push_back(seq);
        } else {
            l2_retained.push_back(seq);
        }
    }

    std::map<std::string, VLogPointer> merged;
    for (uint32_t seq : l1_inputs) {
        auto r = get_l1_reader(seq);
        if (!r) continue;
        for (const auto& e : r->entries()) merged.insert({e.key, e.pointer});
    }
    for (uint32_t seq : l2_inputs) {
        auto r = get_l2_reader(seq);
        if (!r) continue;
        for (const auto& e : r->entries()) merged.insert({e.key, e.pointer});
    }

    // At the coldest demo level, tombstones have fully covered older inputs.
    for (auto it = merged.begin(); it != merged.end(); ) {
        if (is_tombstone(it->second)) it = merged.erase(it);
        else ++it;
    }

    std::vector<uint32_t> new_l2_seqs;
    std::map<std::string, VLogPointer> chunk;
    size_t chunk_size = 0;

    auto flush_chunk = [&]() {
        if (chunk.empty()) return;
        uint32_t seq = store->next_sst_sequence();
        std::string path = store->sst_path(seq);
        if (!SSTableWriter::write(path, chunk)) {
            throw std::runtime_error("[Compaction] Failed to write new L2 SSTable");
        }
        store->add_storage_bytes(24);
        new_l2_seqs.push_back(seq);
        chunk.clear();
        chunk_size = 0;
    };

    for (const auto& [k, v] : merged) {
        chunk[k] = v;
        chunk_size += k.size() + 20;
        store->add_storage_bytes(k.size() + 20);
        if (chunk_size >= store->flush_threshold_bytes()) flush_chunk();
    }
    flush_chunk();

    manifest.version++;
    manifest.l1_seqs.clear();
    manifest.l2_seqs = l2_retained;
    manifest.l2_seqs.insert(manifest.l2_seqs.end(), new_l2_seqs.begin(), new_l2_seqs.end());

    if (!manifest.commit(store->manifest_path())) {
        throw std::runtime_error("[Compaction] Manifest atomic rename failed during L1->L2");
    }

    store->trace_event("compaction:l1-to-l2",
                       "L1 inputs=" + std::to_string(l1_inputs.size()) +
                       " L2 overlaps=" + std::to_string(l2_inputs.size()) +
                       " new L2=" + std::to_string(new_l2_seqs.size()));

    std::cout << "[Compaction] Merged " << l1_inputs.size() << " L1 and "
              << l2_inputs.size() << " L2 files into "
              << new_l2_seqs.size() << " new L2 files.\n";

    std::error_code ec;
    for (uint32_t seq : l1_inputs) std::filesystem::remove(store->sst_path(seq), ec);
    for (uint32_t seq : l2_inputs) std::filesystem::remove(store->sst_path(seq), ec);

    store->load_sstables();
}
