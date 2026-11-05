#pragma once
#include <mem/ptr.h>
struct CPUState {
    Address stack = 900000;
    Address stack_alloc(uint32_t size) { stack -= size; return stack; }
    void stack_free(uint32_t size) { stack += size; }
};
