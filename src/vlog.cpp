#include "vlog.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

// ── Platform abstraction ───────────────────────────────────────
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define vlog_open(p, f, m)    _open(p, f, m)
  #define vlog_write(fd, b, n)  _write(fd, b, static_cast<unsigned int>(n))
  #define vlog_read(fd, b, n)   _read(fd, b, static_cast<unsigned int>(n))
  #define vlog_close(fd)        _close(fd)
  #define vlog_fsync(fd)        _commit(fd)
  #define vlog_lseek(fd, o, w)  _lseeki64(fd, o, w)
  static constexpr int VLOG_APPEND_FLAGS = _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY;
  static constexpr int VLOG_READ_FLAGS   = _O_RDONLY | _O_BINARY;
  static constexpr int VLOG_MODE         = _S_IREAD | _S_IWRITE;
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define vlog_open(p, f, m)    open(p, f, m)
  #define vlog_write(fd, b, n)  write(fd, b, n)
  #define vlog_read(fd, b, n)   read(fd, b, n)
  #define vlog_close(fd)        close(fd)
  #define vlog_fsync(fd)        fdatasync(fd)
  #define vlog_lseek(fd, o, w)  lseek(fd, o, w)
  static constexpr int VLOG_APPEND_FLAGS = O_WRONLY | O_APPEND | O_CREAT;
  static constexpr int VLOG_READ_FLAGS   = O_RDONLY;
  static constexpr int VLOG_MODE         = 0644;
#endif

// ── Helpers ────────────────────────────────────────────────────

static bool vlog_write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        auto n = vlog_write(fd, p, rem);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p   += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

static bool vlog_read_exact(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t rem = len;
    while (rem > 0) {
        auto n = vlog_read(fd, p, rem);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p   += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

// ── VLog implementation ────────────────────────────────────────

VLog::VLog(const std::string& path)
    : path_(path), write_fd_(-1), read_fd_(-1), current_offset_(0) {
    write_fd_ = vlog_open(path_.c_str(), VLOG_APPEND_FLAGS, VLOG_MODE);
    if (write_fd_ < 0) {
        std::cerr << "[VLog] FATAL: cannot open write fd: " << path_ << "\n";
        std::exit(1);
    }

    read_fd_ = vlog_open(path_.c_str(), VLOG_READ_FLAGS, 0);
    if (read_fd_ < 0) {
        std::cerr << "[VLog] FATAL: cannot open read fd: " << path_ << "\n";
        std::exit(1);
    }

    // Initialize current_offset_ from file size (one-time lseek, NOT used per-append).
    auto size = vlog_lseek(write_fd_, 0, SEEK_END);
    current_offset_ = (size > 0) ? static_cast<uint64_t>(size) : 0;
}

VLog::~VLog() {
    if (write_fd_ >= 0) vlog_close(write_fd_);
    if (read_fd_  >= 0) vlog_close(read_fd_);
}

bool VLog::append(const std::string& value, VLogPointer& out_pointer) {
    uint32_t value_size = static_cast<uint32_t>(value.size());

    // Serialize: [value_size][value_bytes]
    std::vector<uint8_t> record(sizeof(uint32_t) + value_size);
    std::memcpy(record.data(), &value_size, sizeof(uint32_t));
    if (value_size > 0)
        std::memcpy(record.data() + sizeof(uint32_t), value.data(), value_size);

    // Capture offset BEFORE write.
    uint64_t write_offset = current_offset_;

    if (!vlog_write_all(write_fd_, record.data(), record.size())) {
        std::cerr << "[VLog] ERROR: write failed\n";
        return false;   // current_offset_ NOT advanced
    }

    // Advance offset AFTER successful write.
    current_offset_ += record.size();

    out_pointer.file_id = 0;
    out_pointer.offset  = write_offset;
    out_pointer.length  = value_size;
    return true;
}

bool VLog::sync() {
    if (vlog_fsync(write_fd_) != 0) {
        std::cerr << "[VLog] ERROR: fsync failed (errno=" << errno << ")\n";
        return false;
    }
    return true;
}

// Read value at pointer.
// Safety: lseek + read on read_fd_ is NOT thread-safe. This is correct for
// Phase 2 (single-threaded). Phase 3+ with concurrent reads MUST use pread()
// on POSIX or per-call file descriptors on Windows to avoid fd position races.
bool VLog::read_at(const VLogPointer& pointer, std::string& out_value) const {
    // Seek to offset on read fd.
    auto pos = vlog_lseek(read_fd_, static_cast<long long>(pointer.offset), SEEK_SET);
    if (pos < 0) return false;

    // Read and validate value_size header.
    uint32_t stored_size = 0;
    if (!vlog_read_exact(read_fd_, &stored_size, sizeof(uint32_t))) return false;
    if (stored_size != pointer.length) return false;   // consistency check

    // Read value bytes.
    out_value.resize(pointer.length);
    if (pointer.length > 0 && !vlog_read_exact(read_fd_, out_value.data(), pointer.length))
        return false;

    return true;
}
