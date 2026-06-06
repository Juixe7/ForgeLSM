#ifndef FLSM_VLOG_H
#define FLSM_VLOG_H

#include <cstdint>
#include <string>

// Pointer to a value stored in the Value Log.
struct VLogPointer {
    uint32_t file_id;   // 0 in Phase 2 (single file)
    uint64_t offset;    // byte offset of record start (value_size field)
    uint32_t length;    // value bytes (excluding 4-byte size header)
};

// Append-only Value Log for WiscKey key-value separation.
//
// Record format: [uint32_t value_size][value_bytes]
//
// Offset is tracked via an internal current_offset_ variable (user-space).
// NEVER derived from lseek on the file descriptor.
class VLog {
public:
    explicit VLog(const std::string& path);
    ~VLog();

    VLog(const VLog&) = delete;
    VLog& operator=(const VLog&) = delete;

    // Append value, return pointer. Returns false on I/O error.
    bool append(const std::string& value, VLogPointer& out_pointer);

    // Flush to stable storage. Returns false on error.
    bool sync();

    // Read value at pointer. Returns false on error.
    bool read_at(const VLogPointer& pointer, std::string& out_value) const;

private:
    std::string path_;
    int         write_fd_;         // persistent fd (append mode)
    int         read_fd_;          // persistent fd (read-only)
    uint64_t    current_offset_;   // user-space offset tracking
};

#endif // FLSM_VLOG_H
