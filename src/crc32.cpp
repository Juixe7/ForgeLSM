#include "crc32.h"
#include <cstring>
#include <vector>

// CRC32 lookup table (IEEE 802.3 polynomial, reflected).
static uint32_t crc32_table[256];
static bool     crc32_table_initialized = false;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

uint32_t compute_crc32(const uint8_t* data, size_t len) {
    if (!crc32_table_initialized) init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

uint32_t record_checksum(uint32_t key_size, uint32_t value_size,
                         const std::string& key, const std::string& value) {
    // Checksum covers: key_size + value_size + key_bytes + value_bytes
    std::vector<uint8_t> buf(sizeof(uint32_t) * 2 + key.size() + value.size());
    size_t off = 0;
    std::memcpy(buf.data() + off, &key_size, sizeof(uint32_t));   off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, &value_size, sizeof(uint32_t)); off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, key.data(), key.size());        off += key.size();
    std::memcpy(buf.data() + off, value.data(), value.size());
    return compute_crc32(buf.data(), buf.size());
}
