#pragma once
#include <cpu/state.h>
#include <functional>
#include <initializer_list>
struct ThreadState {
    std::unique_ptr<CPUState> cpu = std::make_unique<CPUState>();
    std::function<void(Address, Address)> callback;
    void run_callback(Address function, std::initializer_list<Address> args) { callback(function, *args.begin()); }
};
using ThreadStatePtr = std::shared_ptr<ThreadState>;
struct KernelState {
    ThreadStatePtr thread = std::make_shared<ThreadState>();
    ThreadStatePtr get_thread(SceUID) { return thread; }
};
