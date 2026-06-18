#ifndef FLSM_MANIFEST_H
#define FLSM_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

// Tracks the set of SSTables in every LSM level.
// Supports atomic commits for crash-safety (I26).
class Manifest {
public:
    uint32_t version = 0;
    std::vector<std::vector<uint32_t>> levels;

    std::vector<uint32_t>& ensure_level(size_t level);
    const std::vector<uint32_t>& level(size_t level) const;
    size_t level_count() const { return levels.size(); }

    // Load from the given manifest file. Returns true on success.
    bool load(const std::string& path);

    // Atomically commit to the given path.
    // Sequence: write temp -> fsync -> rename to active manifest.
    bool commit(const std::string& path) const;
};

#endif // FLSM_MANIFEST_H
