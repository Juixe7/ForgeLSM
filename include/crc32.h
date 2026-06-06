#ifndef FLSM_CRC32_H
#define FLSM_CRC32_H

#include <cstdint>
#include <string>

// Compute CRC32 over arbitrary byte buffer.
uint32_t compute_crc32(const uint8_t* data, size_t len);

// Compute CRC32 over the WAL record fields:
//   key_size (4 bytes) + value_size (4 bytes) + key + value
// This is the checksum stored alongside each WAL record.
uint32_t record_checksum(uint32_t key_size, uint32_t value_size,
                         const std::string& key, const std::string& value);

#endif // FLSM_CRC32_H
