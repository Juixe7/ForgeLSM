#ifndef FLSM_WAL_H
#define FLSM_WAL_H

#include <cstdint>
#include <string>
#include <vector>

// A single replayed WAL entry.
struct WALEntry {
    std::string key;
    std::string value;
    bool is_tombstone = false;
};

// Result of a WAL replay operation.
struct ReplayResult {
    std::vector<WALEntry> entries;     // valid, checksum-verified records
    bool                  tainted;     // true if replay stopped due to corruption
};

// Write-Ahead Log — append-only, CRC32-validated, crash-safe.
//
// Record format (binary, little-endian, no padding):
//   [uint32_t key_size]
//   [uint32_t value_size]  — NOTE: 0xFFFFFFFF explicitly indicates a TOMBSTONE (delete marker).
//   [uint32_t checksum]    — CRC32 over (key_size, value_size, key, value)
//   [key_size bytes]       — key
//   [value_size bytes]     — value
//
// File descriptor is kept open for the lifetime of the WAL object.
//
// Memory safety note: replay allocates key/value buffers sized by the
// on-disk key_size/value_size fields. These are bounded by MAX_FIELD_SIZE
// (64 MiB) to prevent allocation-based denial-of-service from a corrupted
// WAL. Under normal operation, records are written by this process and
// sizes are trustworthy. The 64 MiB guard exists as a defense-in-depth
// measure — it is NOT a substitute for file-level access control.
class WAL {
public:
    // Opens (or creates) the WAL file at the given path.
    explicit WAL(const std::string& path);

    // Closes the file descriptor.
    ~WAL();

    // Non-copyable, non-movable.
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    // Append a key-value record. Loops until the full record is written.
    // Returns false on I/O error (caller must NOT proceed to memtable).
    bool append(const std::string& key, const std::string& value);

    // Append a tombstone record.
    bool append_delete(const std::string& key);

    // Flush to stable storage (fdatasync / platform equivalent).
    // Returns false if fsync fails (caller must NOT proceed to memtable).
    bool sync();

    // Replay the WAL from byte 0. Returns valid entries + tainted flag.
    // Stops at the first invalid / incomplete / corrupt record.
    // This is a read-only operation — the WAL file is not modified.
    ReplayResult replay() const;

    // True if the last replay encountered corruption before EOF.
    bool is_tainted() const { return tainted_; }

private:
    std::string path_;
    int         fd_;       // persistent file descriptor (append mode)
    bool        tainted_;  // set by replay if corruption detected

    // Size sanity bound — corruption guard, not a product constraint.
    static constexpr uint32_t MAX_FIELD_SIZE = 64u * 1024u * 1024u; // 64 MiB
};

#endif // FLSM_WAL_H
