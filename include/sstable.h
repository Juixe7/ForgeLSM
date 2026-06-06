#ifndef FLSM_SSTABLE_H
#define FLSM_SSTABLE_H

#include "vlog.h"
#include "bloom.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Entry stored in an SSTable: key + vlog pointer.
struct SSTableEntry {
    std::string key;
    VLogPointer pointer;
};

// Writes a sorted set of key-pointer pairs to an SSTable file.
//
// File layout (STRICT):
//   [Data Section: entries in sorted key order]
//   [Bloom Filter Bytes]
//   [Footer: uint32_t entry_count, uint32_t bloom_offset, uint32_t bloom_size, uint32_t checksum]
//
// Entry format:
//   [uint32_t key_size][key bytes][uint32_t file_id][uint64_t offset][uint32_t length]
class SSTableWriter {
public:
    // Write entries to file. Returns false on error.
    static bool write(const std::string& path,
                      const std::map<std::string, VLogPointer>& entries);
};

// Loads and queries an SSTable file.
class SSTableReader {
public:
    SSTableReader() = default;

    // Load from file. Validates footer checksum. Returns false if invalid.
    bool load(const std::string& path);

    // Binary search for key. Returns true and sets out_pointer if found.
    bool get(const std::string& key, VLogPointer& out_pointer) const;

    uint32_t sequence() const { return sequence_; }
    const std::string& path() const { return path_; }

    // Range metadata (assuming entries are sorted).
    const std::string& min_key() const { return entries_.front().key; }
    const std::string& max_key() const { return entries_.back().key; }

    // Returns true if this table's key range overlaps with [min_k, max_k].
    bool overlaps(const std::string& min_k, const std::string& max_k) const {
        if (entries_.empty()) return false;
        return !(max_key() < min_k || min_key() > max_k);
    }

    const std::vector<SSTableEntry>& entries() const { return entries_; }
    const BloomFilter& bloom() const { return bloom_; }

private:
    std::string              path_;
    uint32_t                 sequence_ = 0;
    std::vector<SSTableEntry> entries_;  // sorted by key
    BloomFilter              bloom_;
};

#endif // FLSM_SSTABLE_H
