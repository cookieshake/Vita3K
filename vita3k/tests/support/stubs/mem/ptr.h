#pragma once
// Host fixture boundary: a fixed guest arena with real 32-bit pointer tokens.
// This does not implement the emulator's page table or allocation ownership.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include <util/types.h>
using Address = uint32_t;
struct MemState {
    std::vector<uint8_t> memory = std::vector<uint8_t>(1024 * 1024);
    uint32_t next = 4096;
    uint32_t next_owned = 0x80000;
    bool fail_next_allocation = false;
    std::map<Address, uint32_t> owned_allocations;
    std::map<Address, uint32_t> freed_allocations;
    size_t free_count = 0;
    template <class T, class... Args> T *make(Args &&...args) {
        next = (next + alignof(T) - 1) & ~(alignof(T) - 1);
        assert(next + sizeof(T) < memory.size() / 2);
        T *result = new (memory.data() + next) T(std::forward<Args>(args)...);
        next += sizeof(T);
        return result;
    }
};
inline Address alloc(MemState &mem, uint32_t size, const char *) {
    if (mem.fail_next_allocation) {
        mem.fail_next_allocation = false;
        return 0;
    }
    if (!size || size > UINT32_MAX - 4095u)
        return 0;
    const uint32_t rounded = (size + 4095u) & ~4095u;
    for (auto it = mem.freed_allocations.begin(); it != mem.freed_allocations.end(); ++it) {
        if (it->second >= rounded) {
            const Address result = it->first;
            mem.owned_allocations.emplace(result, rounded);
            mem.freed_allocations.erase(it);
            return result;
        }
    }
    if (mem.next_owned > mem.memory.size() || rounded > mem.memory.size() - mem.next_owned)
        return 0;
    const Address result = mem.next_owned;
    mem.next_owned += rounded;
    try {
        mem.owned_allocations.emplace(result, rounded);
    } catch (...) {
        return 0;
    }
    return result;
}
inline void free(MemState &mem, Address address) {
    const auto it = mem.owned_allocations.find(address);
    if (it != mem.owned_allocations.end()) {
        std::memset(mem.memory.data() + address, 0xdd, it->second);
        mem.freed_allocations.emplace(address, it->second);
        mem.owned_allocations.erase(it);
        ++mem.free_count;
    }
}
template <class T> class Ptr {
    Address addr = 0;
public:
    Ptr() = default;
    explicit Ptr(Address value) : addr(value) {}
    Ptr(T *p, const MemState &mem) : addr(p ? reinterpret_cast<const uint8_t *>(p) - mem.memory.data() : 0) {}
    template <class U> Ptr(const Ptr<U> &other) : addr(other.address()) {}
    Address address() const { return addr; }
    template <class U> Ptr<U> cast() const { return Ptr<U>(addr); }
    T *get(const MemState &mem) const { return addr ? reinterpret_cast<T *>(const_cast<uint8_t *>(mem.memory.data()) + addr) : nullptr; }
    void reset() { addr = 0; }
    explicit operator bool() const { return addr != 0; }
    Ptr &operator=(Address value) { addr = value; return *this; }
};
template <class T, class U> bool operator==(Ptr<T> a, Ptr<U> b) { return a.address() == b.address(); }
template <class T, class U> bool operator<(Ptr<T> a, Ptr<U> b) { return a.address() < b.address(); }
template <class T> Ptr<T> operator+(Ptr<T> base, int32_t offset) { return Ptr<T>(base.address() + offset * sizeof(T)); }
