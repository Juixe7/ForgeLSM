#ifndef FLSM_MEMTABLE_H
#define FLSM_MEMTABLE_H

#include "vlog.h"
#include <map>
#include <string>
#include <cstddef>

// Ordered in-memory key → VLogPointer store backed by std::map.
class Memtable {
public:
    void put(const std::string& key, const VLogPointer& pointer);
    bool get(const std::string& key, VLogPointer& out_pointer) const;

    size_t size() const;
    size_t byte_size() const;   // approximate bytes for flush threshold

    const std::map<std::string, VLogPointer>& entries() const { return table_; }

private:
    std::map<std::string, VLogPointer> table_;
    size_t byte_size_ = 0;
};

#endif // FLSM_MEMTABLE_H
