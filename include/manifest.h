#ifndef FLSM_MANIFEST_H
#define FLSM_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

// Tracks the set of SSTables in L0 and L1.
// Supports atomic commits for crash-safety (I26).
class Manifest {
public:
    uint32_t version = 0;
    std::vector<uint32_t> l0_seqs;
    std::vector<uint32_t> l1_seqs;

    // Load from the given manifest file. Returns true on success.
    bool load(const std::string& path);

    // Atomically commit to the given path.
    // Sequence: write temp -> fsync -> rename to active manifest.
    bool commit(const std::string& path) const;
};

#endif // FLSM_MANIFEST_H
