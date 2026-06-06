#include "memtable.h"

void Memtable::put(const std::string& key, const VLogPointer& pointer) {
    auto [it, inserted] = table_.insert_or_assign(key, pointer);
    if (inserted)
        byte_size_ += key.size() + sizeof(VLogPointer);
}

bool Memtable::get(const std::string& key, VLogPointer& out_pointer) const {
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    out_pointer = it->second;
    return true;
}

size_t Memtable::size() const { return table_.size(); }
size_t Memtable::byte_size() const { return byte_size_; }
