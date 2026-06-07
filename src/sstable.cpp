#include "sstable.h"
#include "crc32.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define sst_open(path, flags, mode)  _open(path, flags, mode)
  #define sst_write(fd, buf, len)      _write(fd, buf, static_cast<unsigned int>(len))
  #define sst_close(fd)                _close(fd)
  #define sst_fsync(fd)                _commit(fd)
  static constexpr int SST_WRITE_FLAGS = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
  static constexpr int SST_MODE        = _S_IREAD | _S_IWRITE;
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define sst_open(path, flags, mode)  open(path, flags, mode)
  #define sst_write(fd, buf, len)      write(fd, buf, len)
  #define sst_close(fd)                close(fd)
  #define sst_fsync(fd)                fdatasync(fd)
  static constexpr int SST_WRITE_FLAGS = O_WRONLY | O_CREAT | O_TRUNC;
  static constexpr int SST_MODE        = 0644;
#endif

static bool write_all_sst(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto written = sst_write(fd, p, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        p += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

static bool fsync_parent_dir(const std::string& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    int fd = open(parent.string().c_str(), O_RDONLY);
    if (fd < 0) return false;
    int rc = fsync(fd);
    close(fd);
    return rc == 0;
#endif
}

// ── SSTableWriter ──────────────────────────────────────────────

bool SSTableWriter::write(const std::string& path,
                          const std::map<std::string, VLogPointer>& entries) {
    // Serialize the data section into a buffer.
    std::vector<uint8_t> data;

    for (const auto& [key, ptr] : entries) {
        uint32_t ks = static_cast<uint32_t>(key.size());
        size_t old = data.size();
        data.resize(old + sizeof(uint32_t) + ks + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t));
        uint8_t* p = data.data() + old;

        std::memcpy(p, &ks, sizeof(uint32_t));           p += sizeof(uint32_t);
        std::memcpy(p, key.data(), ks);                   p += ks;
        std::memcpy(p, &ptr.file_id, sizeof(uint32_t));   p += sizeof(uint32_t);
        std::memcpy(p, &ptr.offset,  sizeof(uint64_t));   p += sizeof(uint64_t);
        std::memcpy(p, &ptr.length,  sizeof(uint32_t));
    }

    // Step 2: Build Bloom Filter
    std::vector<std::string> keys;
    keys.reserve(entries.size());
    for (const auto& [key, ptr] : entries) keys.push_back(key);
    
    BloomFilter bloom;
    bloom.build(keys, 0.01); // 1% false positive target

    uint32_t bloom_offset = static_cast<uint32_t>(data.size());
    uint32_t k = bloom.num_hashes();
    uint32_t bloom_size_total = 4 + static_cast<uint32_t>(bloom.data().size());
    
    size_t old = data.size();
    data.resize(old + bloom_size_total);
    std::memcpy(data.data() + old, &k, 4);
    std::memcpy(data.data() + old + 4, bloom.data().data(), bloom.data().size());

    // Footer: entry_count, bloom_offset, bloom_size, checksum
    uint32_t entry_count = static_cast<uint32_t>(entries.size());
    uint32_t checksum    = compute_crc32(data.data(), data.size());

    const std::string temp_path = path + ".tmp";
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    int fd = sst_open(temp_path.c_str(), SST_WRITE_FLAGS, SST_MODE);
    if (fd < 0) return false;

    bool ok = write_all_sst(fd, data.data(), data.size()) &&
              write_all_sst(fd, &entry_count, sizeof(uint32_t)) &&
              write_all_sst(fd, &bloom_offset, sizeof(uint32_t)) &&
              write_all_sst(fd, &bloom_size_total, sizeof(uint32_t)) &&
              write_all_sst(fd, &checksum, sizeof(uint32_t));

    if (ok && sst_fsync(fd) != 0) ok = false;
    if (sst_close(fd) != 0) ok = false;

    if (!ok) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp_path, path, ec);
    }
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }

    return fsync_parent_dir(path);
}

// ── SSTableReader ──────────────────────────────────────────────

// Extract sequence number from filename like "sst_000001.sst".
static uint32_t parse_sequence(const std::string& path) {
    auto pos = path.rfind("sst_");
    if (pos == std::string::npos) return 0;
    return static_cast<uint32_t>(std::strtoul(path.c_str() + pos + 4, nullptr, 10));
}

bool SSTableReader::load(const std::string& path) {
    path_ = path;
    sequence_ = parse_sequence(path);
    entries_.clear();

    // Read entire file.
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;

    auto file_size = in.tellg();
    if (file_size < 16) return false;   // too small for footer

    // Read footer (last 16 bytes).
    in.seekg(static_cast<std::streamoff>(file_size) - 16, std::ios::beg);
    uint32_t footer[4];
    in.read(reinterpret_cast<char*>(footer), 16);
    if (!in.good()) return false;

    uint32_t entry_count     = footer[0];
    uint32_t bloom_offset    = footer[1];
    uint32_t bloom_size_total= footer[2];
    uint32_t stored_checksum = footer[3];

    if (entry_count == 0) return false;

    size_t payload_size = static_cast<size_t>(static_cast<std::streamoff>(file_size) - 16);
    std::vector<uint8_t> buf(payload_size);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(buf.data()), payload_size);
    if (!in.good()) return false;

    // Validate CRC32 over data section + bloom section.
    uint32_t computed = compute_crc32(buf.data(), payload_size);
    if (computed != stored_checksum) {
        std::cerr << "[SSTable] WARNING: checksum mismatch in " << path << "\n";
        return false;
    }

    // Parse entries from data section.
    size_t off = 0;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (off + sizeof(uint32_t) > bloom_offset) return false;

        uint32_t ks = 0;
        std::memcpy(&ks, buf.data() + off, sizeof(uint32_t)); off += sizeof(uint32_t);

        if (off + ks + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) > bloom_offset)
            return false;

        SSTableEntry e;
        e.key.assign(reinterpret_cast<const char*>(buf.data() + off), ks); off += ks;
        std::memcpy(&e.pointer.file_id, buf.data() + off, sizeof(uint32_t)); off += sizeof(uint32_t);
        std::memcpy(&e.pointer.offset,  buf.data() + off, sizeof(uint64_t)); off += sizeof(uint64_t);
        std::memcpy(&e.pointer.length,  buf.data() + off, sizeof(uint32_t)); off += sizeof(uint32_t);

        entries_.push_back(std::move(e));
    }

    // Init bloom
    if (bloom_size_total >= 4 && bloom_offset + 4 <= payload_size) {
        uint32_t k;
        std::memcpy(&k, buf.data() + bloom_offset, 4);
        uint32_t actual_bloom_size = bloom_size_total - 4;
        bloom_.load(path, bloom_offset + 4, actual_bloom_size, k);
    }

    return true;
}

bool SSTableReader::get(const std::string& key, VLogPointer& out_pointer) const {
    // Binary search on sorted entries.
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
        [](const SSTableEntry& e, const std::string& k) { return e.key < k; });

    if (it != entries_.end() && it->key == key) {
        out_pointer = it->pointer;
        return true;
    }
    return false;
}
