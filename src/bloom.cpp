#include "bloom.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

// MurmurHash64A Implementation
uint64_t hash64(const void* key, int len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;
    uint64_t h = seed ^ (len * m);
    const uint8_t * data = (const uint8_t *)key;
    while(len >= 8) {
        uint64_t k; std::memcpy(&k, data, 8);
        k *= m; k ^= k >> r; k *= m;
        h ^= k; h *= m;
        data += 8; len -= 8;
    }
    switch(len) {
    case 7: h ^= ((uint64_t)data[6]) << 48; [[fallthrough]];
    case 6: h ^= ((uint64_t)data[5]) << 40; [[fallthrough]];
    case 5: h ^= ((uint64_t)data[4]) << 32; [[fallthrough]];
    case 4: h ^= ((uint64_t)data[3]) << 24; [[fallthrough]];
    case 3: h ^= ((uint64_t)data[2]) << 16; [[fallthrough]];
    case 2: h ^= ((uint64_t)data[1]) << 8;  [[fallthrough]];
    case 1: h ^= ((uint64_t)data[0]); h *= m;
    }
    h ^= h >> r; h *= m; h ^= h >> r;
    return h;
}

BloomFilter::~BloomFilter() {
    cleanup();
}

BloomFilter::BloomFilter(BloomFilter&& other) noexcept
    : bits_(std::move(other.bits_)),
      mmap_ptr_(other.mmap_ptr_),
      k_(other.k_),
      m_(other.m_),
      mmap_handle_(other.mmap_handle_),
      mmap_view_(other.mmap_view_),
      mmap_fd_(other.mmap_fd_) {
    other.mmap_ptr_ = nullptr;
    other.mmap_handle_ = nullptr;
    other.mmap_view_ = nullptr;
    other.mmap_fd_ = -1;
}

BloomFilter& BloomFilter::operator=(BloomFilter&& other) noexcept {
    if (this != &other) {
        cleanup();
        bits_ = std::move(other.bits_);
        mmap_ptr_ = other.mmap_ptr_;
        k_ = other.k_;
        m_ = other.m_;
        mmap_handle_ = other.mmap_handle_;
        mmap_view_ = other.mmap_view_;
        mmap_fd_ = other.mmap_fd_;

        other.mmap_ptr_ = nullptr;
        other.mmap_handle_ = nullptr;
        other.mmap_view_ = nullptr;
        other.mmap_fd_ = -1;
    }
    return *this;
}

void BloomFilter::cleanup() {
#ifdef _WIN32
    if (mmap_view_) {
        UnmapViewOfFile(mmap_view_);
        mmap_view_ = nullptr;
    }
    if (mmap_handle_ && mmap_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(mmap_handle_);
        mmap_handle_ = nullptr;
    }
#else
    if (mmap_view_) {
        munmap(mmap_view_, (m_ + 7) / 8);
        mmap_view_ = nullptr;
    }
    if (mmap_fd_ >= 0) {
        close(mmap_fd_);
        mmap_fd_ = -1;
    }
#endif
    mmap_ptr_ = nullptr;
    bits_.clear();
}

void BloomFilter::build(const std::vector<std::string>& keys, double fp_rate) {
    cleanup();
    size_t n = keys.size();
    if (n == 0) { k_ = 0; m_ = 0; return; }

    // Calculate optimal sizing
    double m_calc = - ((double)n * std::log(fp_rate)) / (std::log(2.0) * std::log(2.0));
    m_ = static_cast<uint64_t>(std::ceil(m_calc));
    
    double k_calc = (double)m_ / (double)n * std::log(2.0);
    k_ = static_cast<uint32_t>(std::ceil(k_calc));
    if (k_ == 0) k_ = 1;

    size_t byte_size = (m_ + 7) / 8;
    m_ = byte_size * 8; // Force exactly strict explicit native 8-bit evaluations mathematically aligning disk layouts natively smoothly elegantly fluently
    bits_.assign(byte_size, 0);

    for (const auto& key : keys) {
        uint64_t base = hash64(key.data(), key.size(), 0x9747b28c);
        uint64_t h1 = base;
        uint64_t h2 = (base >> 33) | (base << 31);

        for (uint32_t i = 0; i < k_; ++i) {
            uint64_t idx = (h1 + i * h2) % m_;
            bits_[idx / 8] |= (static_cast<uint8_t>(1) << (idx % 8));
        }
    }
    mmap_ptr_ = bits_.data();
}

bool BloomFilter::load(const std::string& file_path, uint64_t file_offset, uint32_t bloom_size, uint32_t k) {
    cleanup();
    if (bloom_size == 0) return true;

    k_ = k;
    m_ = static_cast<uint64_t>(bloom_size) * 8;

    if (bloom_size < 1024 * 1024) {
        // < 1MB: Load into memory
        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open()) return false;

        in.seekg(file_offset, std::ios::beg);
        bits_.resize(bloom_size);
        in.read(reinterpret_cast<char*>(bits_.data()), bloom_size);

        if (in.gcount() != bloom_size) {
            cleanup();
            return false;
        }

        mmap_ptr_ = bits_.data();
        return true;
    }

    // >= 1MB: MMAP
#ifdef _WIN32
    HANDLE hFile = CreateFileA(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    mmap_handle_ = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile); // CreateFileMapping keeps underlying reference

    if (!mmap_handle_) return false;

    // Calculate mapping offset explicitly matching Windows allocation granularity (often 64KB).
    // For simplicity of an explicit embedded byte offset, we map the entire space up to bloom_size.
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD alloc_gran = sysInfo.dwAllocationGranularity;
    
    uint64_t view_offset = (file_offset / alloc_gran) * alloc_gran;
    uint32_t offset_diff = static_cast<uint32_t>(file_offset - view_offset);

    mmap_view_ = MapViewOfFile(mmap_handle_, FILE_MAP_READ, (DWORD)(view_offset >> 32), (DWORD)(view_offset & 0xFFFFFFFF), bloom_size + offset_diff);
    
    if (!mmap_view_) {
        cleanup();
        return false;
    }
    
    mmap_ptr_ = static_cast<const uint8_t*>(mmap_view_) + offset_diff;
#else
    mmap_fd_ = open(file_path.c_str(), O_RDONLY);
    if (mmap_fd_ < 0) return false;

    long page_size = sysconf(_SC_PAGE_SIZE);
    uint64_t view_offset = (file_offset / page_size) * page_size;
    uint32_t offset_diff = file_offset - view_offset;

    mmap_view_ = mmap(NULL, bloom_size + offset_diff, PROT_READ, MAP_SHARED, mmap_fd_, view_offset);
    if (mmap_view_ == MAP_FAILED) {
        mmap_view_ = nullptr;
        cleanup();
        return false;
    }

    mmap_ptr_ = static_cast<const uint8_t*>(mmap_view_) + offset_diff;
#endif

    return true;
}

bool BloomFilter::may_contain(const std::string& key) const {
    const uint8_t* ptr = mmap_view_ ? mmap_ptr_ : (bits_.empty() ? nullptr : bits_.data());
    if (!ptr || m_ == 0 || k_ == 0) return true; // Safe fallback (false positive equivalent)

    uint64_t base = hash64(key.data(), key.size(), 0x9747b28c);
    uint64_t h1 = base;
    uint64_t h2 = (base >> 33) | (base << 31);

    for (uint32_t i = 0; i < k_; ++i) {
        uint64_t idx = (h1 + i * h2) % m_;
        if (!(ptr[idx / 8] & (static_cast<uint8_t>(1) << (idx % 8)))) {
            return false; // Definitely not present
        }
    }
    return true; // May be present
}
