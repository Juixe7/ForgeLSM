#include "manifest.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #define m_open(path, flags, mode)  _open(path, flags, mode)
  #define m_write(fd, buf, len)      _write(fd, buf, static_cast<unsigned int>(len))
  #define m_close(fd)                _close(fd)
  #define m_fsync(fd)                _commit(fd)
  static constexpr int M_FLAGS = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
  static constexpr int M_MODE  = _S_IREAD | _S_IWRITE;
  #ifndef EINTR
    #define EINTR 0
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #define m_open(path, flags, mode)  open(path, flags, mode)
  #define m_write(fd, buf, len)      write(fd, buf, len)
  #define m_close(fd)                close(fd)
  #define m_fsync(fd)                fdatasync(fd)
  static constexpr int M_FLAGS = O_WRONLY | O_CREAT | O_TRUNC;
  static constexpr int M_MODE  = 0644;
#endif

// Raw write_all helper mirroring WAL
static bool write_all_m(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto written = m_write(fd, p, remaining);
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

std::vector<uint32_t>& Manifest::ensure_level(size_t level) {
    if (levels.size() <= level) levels.resize(level + 1);
    return levels[level];
}

const std::vector<uint32_t>& Manifest::level(size_t level) const {
    static const std::vector<uint32_t> empty;
    return level < levels.size() ? levels[level] : empty;
}

bool Manifest::load(const std::string& path) {
    if (!std::filesystem::exists(path)) return false;

    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string token;
    uint32_t v = 0;
    if (!(in >> token >> v) || token != "VERSION") return false;
    version = v;

    levels.clear();

    while (in >> token) {
        if (token.size() >= 2 && token[0] == 'L') {
            char* end = nullptr;
            unsigned long level_id = std::strtoul(token.c_str() + 1, &end, 10);
            if (!end || *end != '\0') return false;
            size_t count;
            if (!(in >> count)) return false;
            auto& seqs = ensure_level(static_cast<size_t>(level_id));
            for (size_t i = 0; i < count; ++i) {
                uint32_t seq;
                if (in >> seq) seqs.push_back(seq);
                else return false;
            }
        }
    }
    return true;
}

bool Manifest::commit(const std::string& path) const {
    std::string temp_path = path + ".tmp";
    
    // Generate manifest payload
    std::ostringstream oss;
    oss << "VERSION " << version << "\n";
    for (size_t level = 0; level < levels.size(); ++level) {
        oss << "L" << level << " " << levels[level].size() << "\n";
        for (uint32_t seq : levels[level]) oss << seq << " ";
        oss << "\n";
    }

    std::string payload = oss.str();

    // Write -> Fsync -> Close for real durability
    int fd = m_open(temp_path.c_str(), M_FLAGS, M_MODE);
    if (fd < 0) return false;

    if (!write_all_m(fd, payload.data(), payload.size())) {
        m_close(fd);
        return false;
    }

    if (m_fsync(fd) != 0) { // STRICT DURABILITY GUARANTEE
        m_close(fd);
        return false;
    }
    m_close(fd);

    // Atomic rename over the old manifest file.
    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    if (ec) { // Fallback for some Windows configurations where target must not exist
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp_path, path, ec);
    }
    return !ec;
}
