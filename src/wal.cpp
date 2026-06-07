#include "wal.h"
#include "crc32.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

// ── Platform abstraction for raw file I/O ──────────────────────
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define wal_open(path, flags, mode)  _open(path, flags, mode)
  #define wal_write(fd, buf, len)      _write(fd, buf, static_cast<unsigned int>(len))
  #define wal_read(fd, buf, len)       _read(fd, buf, static_cast<unsigned int>(len))
  #define wal_close(fd)                _close(fd)
  #define wal_fsync(fd)                _commit(fd)
  #define wal_lseek(fd, off, whence)   _lseeki64(fd, off, whence)
  static constexpr int WAL_APPEND_FLAGS = _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY;
  static constexpr int WAL_RDWR_FLAGS   = _O_RDWR | _O_BINARY;
  static constexpr int WAL_READ_FLAGS   = _O_RDONLY | _O_BINARY;
  static constexpr int WAL_MODE         = _S_IREAD | _S_IWRITE;
  // Windows does not use EINTR; define it away for uniform code.
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define wal_open(path, flags, mode)  open(path, flags, mode)
  #define wal_write(fd, buf, len)      write(fd, buf, len)
  #define wal_read(fd, buf, len)       read(fd, buf, len)
  #define wal_close(fd)                close(fd)
  #define wal_fsync(fd)                fdatasync(fd)
  #define wal_lseek(fd, off, whence)   lseek(fd, off, whence)
  static constexpr int WAL_APPEND_FLAGS = O_WRONLY | O_APPEND | O_CREAT;
  static constexpr int WAL_RDWR_FLAGS   = O_RDWR;
  static constexpr int WAL_READ_FLAGS   = O_RDONLY;
  static constexpr int WAL_MODE         = 0644;
#endif

// ── Helper: write all bytes, retrying on EINTR and short writes ─
static bool write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto written = wal_write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;   // interrupted — retry
            return false;                   // real I/O error
        }
        if (written == 0) return false;     // unexpected zero-write
        p         += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

// ── Helper: read exactly `len` bytes; returns false on short/EOF ─
static bool read_exact(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto n = wal_read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted — retry
            return false;                   // real I/O error
        }
        if (n == 0) return false;           // EOF
        p         += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

static bool truncate_file_to(const std::string& path, uint64_t offset) {
#ifdef _WIN32
    int fd = wal_open(path.c_str(), WAL_RDWR_FLAGS, WAL_MODE);
    if (fd < 0) return false;
    int rc = _chsize_s(fd, offset);
    if (rc == 0) rc = wal_fsync(fd);
    wal_close(fd);
    return rc == 0;
#else
    if (::truncate(path.c_str(), static_cast<off_t>(offset)) != 0) return false;
    int fd = wal_open(path.c_str(), WAL_RDWR_FLAGS, WAL_MODE);
    if (fd < 0) return false;
    int rc = wal_fsync(fd);
    wal_close(fd);
    return rc == 0;
#endif
}

// ── WAL constructor ────────────────────────────────────────────
WAL::WAL(const std::string& path) : path_(path), fd_(-1), tainted_(false) {
    fd_ = wal_open(path_.c_str(), WAL_APPEND_FLAGS, WAL_MODE);
    if (fd_ < 0) {
        std::cerr << "[WAL] FATAL: cannot open " << path_ << "\n";
        std::exit(1);
    }
}

// ── WAL destructor ─────────────────────────────────────────────
WAL::~WAL() {
    if (fd_ >= 0) wal_close(fd_);
}

// ── append ─────────────────────────────────────────────────────
bool WAL::append(const std::string& key, const std::string& value) {
    uint32_t key_size   = static_cast<uint32_t>(key.size());
    uint32_t value_size = static_cast<uint32_t>(value.size());
    uint32_t checksum   = record_checksum(key_size, value_size, key, value);

    // Serialize the full record into a single buffer to minimize syscalls.
    const size_t record_len = sizeof(uint32_t) * 3 + key_size + value_size;
    std::vector<uint8_t> record(record_len);
    size_t off = 0;
    std::memcpy(record.data() + off, &key_size,   sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, &value_size, sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, &checksum,   sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, key.data(),   key_size);        off += key_size;
    std::memcpy(record.data() + off, value.data(), value_size);

    // Loop until the entire record is written (handles EINTR + short writes).
    if (!write_all(fd_, record.data(), record_len)) {
        std::cerr << "[WAL] ERROR: failed to write record\n";
        return false;
    }
    return true;
}

// ── append_delete ──────────────────────────────────────────────
bool WAL::append_delete(const std::string& key) {
    uint32_t key_size   = static_cast<uint32_t>(key.size());
    uint32_t value_size = 0xFFFFFFFF; // Tombstone marker
    uint32_t checksum   = record_checksum(key_size, value_size, key, "");

    const size_t record_len = sizeof(uint32_t) * 3 + key_size;
    std::vector<uint8_t> record(record_len);
    size_t off = 0;
    std::memcpy(record.data() + off, &key_size,   sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, &value_size, sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, &checksum,   sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(record.data() + off, key.data(),   key_size);

    if (!write_all(fd_, record.data(), record_len)) {
        std::cerr << "[WAL] ERROR: failed to write tombstone\n";
        return false;
    }
    return true;
}

// ── sync ───────────────────────────────────────────────────────
bool WAL::sync() {
    if (wal_fsync(fd_) != 0) {
        std::cerr << "[WAL] ERROR: fsync failed (errno=" << errno << ")\n";
        return false;
    }
    return true;
}

// ── replay ─────────────────────────────────────────────────────
//
// Memory safety: key_size and value_size are bounded by MAX_FIELD_SIZE
// (64 MiB) before any allocation. This prevents a corrupted WAL from
// causing unbounded memory consumption. See class-level comment in wal.h.
ReplayResult WAL::replay() const {
    ReplayResult result;
    result.tainted = false;
    result.safe_offset = 0;
    result.bad_offset = 0;

    int rfd = wal_open(path_.c_str(), WAL_READ_FLAGS, 0);
    if (rfd < 0) return result;   // file does not exist yet

    auto file_size_raw = wal_lseek(rfd, 0, SEEK_END);
    uint64_t file_size = file_size_raw > 0 ? static_cast<uint64_t>(file_size_raw) : 0;
    wal_lseek(rfd, 0, SEEK_SET);

    bool hit_eof_cleanly = false;

    while (true) {
        uint32_t key_size = 0, value_size = 0, stored_checksum = 0;
        auto record_start_raw = wal_lseek(rfd, 0, SEEK_CUR);
        uint64_t record_start = record_start_raw >= 0 ? static_cast<uint64_t>(record_start_raw) : result.safe_offset;

        // Read 12-byte header. A clean EOF here means we've consumed
        // all records — NOT corruption.
        if (!read_exact(rfd, &key_size, sizeof(uint32_t))) {
            hit_eof_cleanly = (record_start == file_size);
            if (!hit_eof_cleanly) result.bad_offset = record_start;
            break;
        }
        if (!read_exact(rfd, &value_size, sizeof(uint32_t))) {
            result.bad_offset = record_start;
            break;
        }
        if (!read_exact(rfd, &stored_checksum, sizeof(uint32_t))) {
            result.bad_offset = record_start;
            break;
        }

        // Size sanity check before allocation.
        if (key_size > MAX_FIELD_SIZE ||
            (value_size != 0xFFFFFFFF && value_size > MAX_FIELD_SIZE)) {
            result.bad_offset = record_start;
            break;
        }

        // Read key.
        std::string key(key_size, '\0');
        if (key_size > 0 && !read_exact(rfd, key.data(), key_size)) {
            result.bad_offset = record_start;
            break;
        }

        // Check if tombstone
        if (value_size == 0xFFFFFFFF) {
            uint32_t expected = record_checksum(key_size, value_size, key, "");
            if (stored_checksum != expected) {
                result.bad_offset = record_start;
                break;
            }
            result.entries.push_back({std::move(key), "", true});
            auto next_raw = wal_lseek(rfd, 0, SEEK_CUR);
            if (next_raw >= 0) result.safe_offset = static_cast<uint64_t>(next_raw);
            continue;
        }

        // Read value.
        std::string value(value_size, '\0');
        if (value_size > 0 && !read_exact(rfd, value.data(), value_size)) {
            result.bad_offset = record_start;
            break;
        }

        // Verify checksum.
        uint32_t expected = record_checksum(key_size, value_size, key, value);
        if (stored_checksum != expected) {
            result.bad_offset = record_start;
            break;
        }

        result.entries.push_back({std::move(key), std::move(value), false});
        auto next_raw = wal_lseek(rfd, 0, SEEK_CUR);
        if (next_raw >= 0) result.safe_offset = static_cast<uint64_t>(next_raw);
        continue;
    }

    wal_close(rfd);

    // If we didn't exit cleanly at a record boundary, the WAL is tainted.
    if (!hit_eof_cleanly) {
        result.tainted = true;
        std::cerr << "[WAL] WARNING: replay stopped at corrupt/incomplete record "
                  << "at byte " << result.bad_offset
                  << " (recovered " << result.entries.size()
                  << " valid entries, truncating to byte " << result.safe_offset << ")\n";
        if (!truncate_file_to(path_, result.safe_offset)) {
            std::cerr << "[WAL] WARNING: failed to truncate corrupt WAL tail for "
                      << path_ << "\n";
        }
    }

    // Cache the tainted state on the WAL object.
    const_cast<WAL*>(this)->tainted_ = result.tainted;

    return result;
}
