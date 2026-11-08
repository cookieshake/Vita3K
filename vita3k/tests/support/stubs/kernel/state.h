#pragma once
#include <cpu/state.h>
#include <functional>
#include <initializer_list>
#include <cstdint>
struct ThreadState {
    std::unique_ptr<CPUState> cpu = std::make_unique<CPUState>();
    std::function<void(Address, Address)> callback;
    uint32_t call_guest(Address function, std::initializer_list<uint32_t> args) { callback(function, *args.begin()); return 0; }
};
using ThreadStatePtr = std::shared_ptr<ThreadState>;
struct KernelState {
    ThreadStatePtr thread = std::make_shared<ThreadState>();
    ThreadStatePtr get_thread(SceUID) { return thread; }
};
