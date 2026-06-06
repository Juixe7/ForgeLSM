#ifndef FLSM_VLOG_GC_H
#define FLSM_VLOG_GC_H

class KVStore;

// Runs Value Log Garbage Collection on the KVStore.
//
// 1. Rotates the current VLog to a GC target file.
// 2. Iterates the entire LSM tree to find strictly the newest live versions of keys.
// 3. Reads live values from the target old VLog.
// 4. Rewrites them via standard `store->put(key, value)`.
// 5. Deletes the old VLog safely.
void run_vlog_gc(KVStore* store);

#endif // FLSM_VLOG_GC_H
