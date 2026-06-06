#ifndef FLSM_BLOOM_H
#define FLSM_BLOOM_H

#include <string>
#include <vector>
#include <cstdint>

// Deterministic 64-bit cross-platform hash (MurmurHash64A)
uint64_t hash64(const void* key, int len, uint64_t seed);

class BloomFilter {
public:
    BloomFilter() : k_(0), m_(0), mmap_handle_(nullptr), mmap_view_(nullptr) {}
    ~BloomFilter();

    // Disable copy
    BloomFilter(const BloomFilter&) = delete;
    BloomFilter& operator=(const BloomFilter&) = delete;

    // Allow move
    BloomFilter(BloomFilter&& other) noexcept;
    BloomFilter& operator=(BloomFilter&& other) noexcept;

    // Builder method (called during flush/compaction)
    void build(const std::vector<std::string>& keys, double fp_rate = 0.01);

    // Load method (called by SSTableReader)
    // Loads either into heap memory (< 1MB) or via mmap (>= 1MB)
    bool load(const std::string& file_path, uint64_t file_offset, uint32_t bloom_size, uint32_t k);

    // Query method
    bool may_contain(const std::string& key) const;

    // Serialization getters
    const std::vector<uint8_t>& data() const { return bits_; }
    uint32_t num_hashes() const { return k_; }

private:
    void cleanup();

    std::vector<uint8_t> bits_;    // Heap storage (<1MB) or builder storage
    const uint8_t* mmap_ptr_ = nullptr; // Pointer to actual bloom bytes (either bits_.data() or mmap_view_)
    uint32_t k_;                   // Number of hash functions
    uint64_t m_;                   // Number of explicit bits

    void* mmap_handle_;            // Windows file mapping handle
    void* mmap_view_;              // Windows mapped view pointer
    int   mmap_fd_ = -1;           // POSIX fd for mmap (if applicable)
};

#endif // FLSM_BLOOM_H
