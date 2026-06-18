#ifndef FLSM_COMPACTION_H
#define FLSM_COMPACTION_H

#include <cstddef>

class KVStore;

// Runs compaction between adjacent levels. L0 compacts to L1, L1 to L2, and so on.
// Tombstones are preserved because deeper dynamic levels may still contain older values.
void run_level_compaction(KVStore* store, size_t source_level);
void run_compaction(KVStore* store);
void run_l1_to_l2_compaction(KVStore* store);

#endif // FLSM_COMPACTION_H
