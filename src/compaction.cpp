#include "compaction.h"
#include "kvstore.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {

const SSTableReader* find_reader(const std::vector<SSTableReader>& readers, uint32_t seq) {
    for (const auto& reader : readers) {
        if (reader.sequence() == seq) return &reader;
    }
    return nullptr;
}

} // namespace

void run_level_compaction(KVStore* store, size_t source_level) {
    if (!store) return;

    auto& manifest = store->manifest_;
    const size_t target_level = source_level + 1;
    manifest.ensure_level(source_level);
    manifest.ensure_level(target_level);
    auto& source_seqs = manifest.ensure_level(source_level);
    if (source_seqs.empty()) return;
    if (store->levels_sstables_.size() <= target_level) {
        store->levels_sstables_.resize(target_level + 1);
    }

    const auto source_inputs = source_seqs;
    std::string global_min = "\xFF";
    std::string global_max;

    auto& source_readers = store->levels_sstables_[source_level];
    auto& target_readers = store->levels_sstables_[target_level];

    for (uint32_t seq : source_inputs) {
        auto reader = find_reader(source_readers, seq);
        if (!reader) continue;
        if (reader->min_key() < global_min) global_min = reader->min_key();
        if (reader->max_key() > global_max) global_max = reader->max_key();
    }

    std::vector<uint32_t> target_inputs;
    std::vector<uint32_t> target_retained;
    for (uint32_t seq : manifest.level(target_level)) {
        auto reader = find_reader(target_readers, seq);
        if (!reader) continue;
        if (reader->overlaps(global_min, global_max)) {
            target_inputs.push_back(seq);
        } else {
            target_retained.push_back(seq);
        }
    }

    std::map<std::string, VLogPointer> merged;

    if (source_level == 0) {
        // L0 files overlap and are append-ordered in the manifest, so reverse gives newest first.
        for (auto it = source_inputs.rbegin(); it != source_inputs.rend(); ++it) {
            auto reader = find_reader(source_readers, *it);
            if (!reader) continue;
            for (const auto& entry : reader->entries()) {
                merged.insert({entry.key, entry.pointer});
            }
        }
    } else {
        for (uint32_t seq : source_inputs) {
            auto reader = find_reader(source_readers, seq);
            if (!reader) continue;
            for (const auto& entry : reader->entries()) {
                merged.insert({entry.key, entry.pointer});
            }
        }
    }

    for (uint32_t seq : target_inputs) {
        auto reader = find_reader(target_readers, seq);
        if (!reader) continue;
        for (const auto& entry : reader->entries()) {
            merged.insert({entry.key, entry.pointer});
        }
    }

    // Preserve tombstones at every dynamic level. Older values may exist below the
    // target level now or after future level expansion.
    std::vector<uint32_t> new_target_seqs;
    std::map<std::string, VLogPointer> chunk;
    size_t chunk_size = 0;

    auto flush_chunk = [&]() {
        if (chunk.empty()) return;
        uint32_t seq = store->next_sst_sequence();
        std::string path = store->sst_path(seq);
        if (!SSTableWriter::write(path, chunk)) {
            throw std::runtime_error("[Compaction] Failed to write L" + std::to_string(target_level) + " SSTable");
        }
        store->add_storage_bytes(24);
        new_target_seqs.push_back(seq);
        chunk.clear();
        chunk_size = 0;
    };

    for (const auto& [key, pointer] : merged) {
        chunk[key] = pointer;
        chunk_size += key.size() + 20;
        store->add_storage_bytes(key.size() + 20);
        if (chunk_size >= store->flush_threshold_bytes()) flush_chunk();
    }
    flush_chunk();

    manifest.version++;
    source_seqs.clear();
    auto& target_seqs = manifest.ensure_level(target_level);
    target_seqs = target_retained;
    target_seqs.insert(target_seqs.end(), new_target_seqs.begin(), new_target_seqs.end());

    if (!manifest.commit(store->manifest_path())) {
        throw std::runtime_error("[Compaction] Manifest atomic rename failed during L" +
                                 std::to_string(source_level) + "->L" +
                                 std::to_string(target_level));
    }

    store->trace_event("compaction:l" + std::to_string(source_level) + "-to-l" + std::to_string(target_level),
                       "source_inputs=" + std::to_string(source_inputs.size()) +
                       " target_overlaps=" + std::to_string(target_inputs.size()) +
                       " new_target=" + std::to_string(new_target_seqs.size()));

    std::cout << "[Compaction] Merged " << source_inputs.size() << " L" << source_level
              << " and " << target_inputs.size() << " L" << target_level
              << " files into " << new_target_seqs.size()
              << " new L" << target_level << " files.\n";

    std::error_code ec;
    for (uint32_t seq : source_inputs) std::filesystem::remove(store->sst_path(seq), ec);
    for (uint32_t seq : target_inputs) std::filesystem::remove(store->sst_path(seq), ec);

    store->load_sstables();
}

void run_compaction(KVStore* store) {
    run_level_compaction(store, 0);
}

void run_l1_to_l2_compaction(KVStore* store) {
    run_level_compaction(store, 1);
}
