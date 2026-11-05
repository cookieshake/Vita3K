#include <ngs/state.h>
#include <ngs/system.h>
#include <ngs/modules/mixer.h>
#include <ngs/modules/output.h>
#include <ngs/modules/atrac9.h>
#include <kernel/state.h>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *name) { if (!ok) throw std::runtime_error(name); }
static int cleaned = 0;
static std::function<void()> before_cleanup;
static uint32_t actual_parameter_bytes = 0;
static size_t configured_module_count = 1;
static int definition_calls = 0;
static int throw_on_definition_call = 0;
static int initialization_calls = 0;
static int throw_on_initialization_call = 0;

namespace ngs { void Atrac9Module::free_swr_contexts() {} }

struct ProbeModule : ngs::Module {
    bool process(KernelState &, const MemState &, SceUID, ngs::ModuleData &,
        std::unique_lock<std::recursive_mutex> &, std::unique_lock<std::mutex> &) override { return false; }
    uint32_t get_buffer_parameter_size() const override { return actual_parameter_bytes; }
    void initialize_voice_data(ngs::ModuleData &data) const override {
        ++initialization_calls;
        if (initialization_calls == throw_on_initialization_call)
            throw std::runtime_error("injected module initialization failure");
        ngs::Module::initialize_voice_data(data);
    }
    void cleanup_voice_state(ngs::ModuleData &) override {
        if (before_cleanup) before_cleanup();
        ++cleaned;
    }
};

// Only construction of module definitions/ATRAC decoding is substituted.
namespace ngs {
void voice_definition_init(State &, MemState &) {}
void apply_voice_definition(VoiceDefinition *definition, std::vector<std::unique_ptr<Module>> &modules) {
    if (definition->type == BussType::BUSS_MIXER) {
        modules.push_back(std::make_unique<InputMixerModule>());
        return;
    }
    if (definition->type == BussType::BUSS_MASTER) {
        modules.push_back(std::make_unique<InputMixerModule>());
        modules.push_back(std::make_unique<OutputModule>());
        return;
    }
    ++definition_calls;
    if (definition_calls == throw_on_definition_call)
        throw std::runtime_error("injected definition failure");
    for (size_t i = 0; i < configured_module_count; ++i)
        modules.push_back(std::make_unique<ProbeModule>());
}
uint32_t get_voice_definition_size(VoiceDefinition *) { return actual_parameter_bytes * configured_module_count; }
}
struct Env {
    struct { struct { bool ngs_enable = true; } current_config; } cfg;
    ngs::State &ngs;
    MemState &mem;
};
#define RET_ERROR(value) (value)
#define TRACY_FUNC(...) ((void)0)
#include "ngs_test_adapter.h"
int run_ngs_review_fixture();
static ngs::Rack *owned_rack(MemState &mem, ngs::State &state, ngs::System *sys, uint32_t caller_buffer, uint32_t caller_size) {
    auto definition = mem.make<ngs::VoiceDefinition>();
    definition->output_count = 2;
    SceNgsRackDescription description{};
    description.definition = Ptr<ngs::VoiceDefinition>(definition, mem);
    description.voice_count = 2;
    description.channels_per_voice = 2;
    description.patches_per_output = 1;
    description.unk14 = Ptr<void>(0x3456);
    uint32_t required = 0;
    Env env{{}, state, mem};
    check(call_rack_size(env, sys, &description, &required) == 0, "required size export query");
    check(required <= caller_size, "caller buffer declared size is sufficient");
    std::fill(mem.memory.begin() + caller_buffer, mem.memory.begin() + caller_buffer + caller_size, 0xA5);
    SceNgsBufferInfo info{ Ptr<void>(caller_buffer), caller_size };
    Ptr<ngs::Rack> handle;
    check(call_rack_init(env, sys, &info, &description, &handle) == SCE_NGS_OK, "rack relocation export init");
    check(handle && handle.address() != caller_buffer, "returned handle points at relocated rack");
    check(handle.address() % 4096 == 0, "owned backing starts page aligned");
    check(handle.get(mem)->memspace.address() == handle.address(), "rack pool uses owned backing");
    check(handle.get(mem)->voices[0].address() % alignof(ngs::Voice) == 0, "voice host object alignment");
    check(handle.get(mem)->voices[0].get(mem)->datas[0].info.data.address() % 4 == 0, "parameter pool alignment");
    check(std::all_of(mem.memory.begin() + caller_buffer, mem.memory.begin() + caller_buffer + caller_size,
        [](uint8_t value) { return value == 0xA5; }), "caller buffer and adjacent canary stay untouched");
    check(state.rack_owned_blocks.contains(handle.get(mem)), "host State records block ownership");
    return handle.get(mem);
}

static ngs::System *system(MemState &mem, ngs::State &state) {
    auto sys = mem.make<ngs::System>(Ptr<void>(), 0);
    sys->granularity = 64;
    state.systems.push_back(sys);
    return sys;
}
static ngs::Rack *rack(MemState &mem, ngs::State &state, ngs::System *sys) {
    auto r = mem.make<ngs::Rack>(sys, Ptr<void>(), 0);
    r->modules.push_back(std::make_unique<ProbeModule>());
    r->patches_per_output = 1;
    r->channels_per_voice = 2;
    auto v = mem.make<ngs::Voice>();
    v->init(r);
    v->frame_count = 0;
    r->voices.emplace_back(v, mem);
    sys->racks.push_back(r);
    state.rack_user_data[r] = Ptr<void>(0x1234);
    return r;
}
static ngs::Patch *patch(MemState &mem, ngs::Rack *src, ngs::Rack *dest, int port) {
    auto p = mem.make<ngs::Patch>();
    p->source = src->voices[0];
    p->dest = dest->voices[0];
    p->output_index = port;
    p->output_sub_index = 0;
    p->dest_index = 0;
    src->voices[0].get(mem)->patches[port][0] = Ptr<ngs::Patch>(p, mem);
    return p;
}
int main() {
    const char *phase = "construction";
    try {
        MemState mem;
        ngs::State state;
        auto sys = system(mem, state);
        auto a = rack(mem, state, sys), b = rack(mem, state, sys), c = rack(mem, state, sys);
        auto ab = patch(mem, a, b, 0), ba = patch(mem, b, a, 0), ac = patch(mem, a, c, 1);
        const auto bvoice = b->voices[0].get(mem);
        sys->voice_scheduler.queue = {bvoice, c->voices[0].get(mem), bvoice};
        before_cleanup = [&] {
            check(!ab->is_active() && !ab->dest && !ab->source, "inbound removed BEFORE voice cleanup");
            check(!ba->is_active() && !ba->dest, "outbound removed BEFORE voice cleanup");
            check(std::find(sys->voice_scheduler.queue.begin(), sys->voice_scheduler.queue.end(), bvoice) == sys->voice_scheduler.queue.end(), "all queue references removed");
            check(ac->is_active(), "unrelated route survives");
        };
        phase = "release incoming/outgoing routes";
        ngs::release_rack(state, mem, sys, b);
        before_cleanup = {};
        check(cleaned == 1 && !state.rack_user_data.contains(b), "cleanup once and metadata removed");
        ngs::release_rack(state, mem, sys, b);
        check(cleaned == 1, "duplicate internal release ignored");
        phase = "system shutdown";
        ngs::release_system(state, mem, sys);
        check(cleaned == 3 && state.systems.empty() && state.rack_user_data.empty(), "system uses same cleanup");

        phase = "callback construction";
        auto sys2 = system(mem, state);
        auto d = rack(mem, state, sys2), e = rack(mem, state, sys2);
        const Address dh = Ptr<void>(d, mem).address(), eh = Ptr<void>(e, mem).address();
        Env env{{}, state, mem};
        state.rack_user_data[d] = Ptr<void>(0xfeed);
        state.rack_user_data[e] = Ptr<void>(0xbeef);
        KernelState kernel;
        int callbacks = 0;
        kernel.thread->callback = [&](Address function, Address argument) {
            check(sys2->voice_scheduler.is_update_thread(), "callback identifies own update thread");
            phase = "callback update";
        sys2->voice_scheduler.update(kernel, mem, 1); // must not recurse
            const auto &info = *Ptr<SceNgsCallbackInfo>(argument).get(mem);
            check(argument != dh && argument != eh, "callback receives info pointer");
            check(!info.voice_handle && info.module_id == 0 && info.callback_reason == 0 && !info.callback_ptr, "zero release callback fields");
            check(sys2->voice_scheduler.operations_pending.empty(), "pop before reentry");
            if (++callbacks == 1) {
                check(function == 0x100 && info.rack_handle.address() == dh && info.userdata.address() == 0xfeed, "first callback snapshot");
                check(!state.rack_user_data.contains(d), "destroy before completion");
                check(call_rack_release(env, e, Ptr<void>()) == SCE_NGS_ERROR_RESOURCE_LOCKED, "synchronous release from update thread rejected");
                check(call_system_release(env, sys2) == SCE_NGS_ERROR_RESOURCE_LOCKED, "system release from callback rejected");
                check(call_rack_release(env, e, Ptr<void>(0x200)) == SCE_NGS_OK, "callback can enqueue next rack release");
            } else {
                check(function == 0x200 && info.rack_handle.address() == eh && info.userdata.address() == 0xbeef, "reentrant completion");
            }
        };
        check(call_rack_release(env, d, Ptr<void>(0x100)) == SCE_NGS_OK, "outside-update asynchronous request accepted");
        check(state.rack_user_data.contains(d), "outside-update request waits for next update");
        check(call_rack_release(env, d, Ptr<void>()) == SCE_NGS_ERROR_INVALID_STATE, "duplicate release rejected");
        check(call_system_release(env, sys2) == SCE_NGS_ERROR_RESOURCE_LOCKED, "pending callback blocks system destruction");
        phase = "callback update";
        sys2->voice_scheduler.update(kernel, mem, 1);
        check(callbacks == 2 && kernel.thread->cpu->stack == 900000 && !sys2->voice_scheduler.is_updating && !sys2->voice_scheduler.is_update_thread(), "stack/update restored");
        check(state.rack_user_data.empty(), "no System-lifetime metadata retention");
        ngs::release_system(state, mem, sys2);
        auto left = system(mem, state), right = system(mem, state);
        auto lr = rack(mem, state, left), rr = rack(mem, state, right);
        SceNgsPatchSetupInfo setup{ lr->voices[0], 0, 0, rr->voices[0], 0 };
        check(!left->voice_scheduler.patch(mem, &setup), "cross-system route rejected before allocation");
        setup.source.reset();
        check(!left->voice_scheduler.patch(mem, &setup), "null route source rejected");
        ngs::release_system(state, mem, left);
        ngs::release_system(state, mem, right);
        phase = "churn";
        auto sys3 = system(mem, state);
        for (int i = 0; i < 300; ++i) {
            auto r = rack(mem, state, sys3);
            ngs::release_rack(state, mem, sys3, r);
            check(sys3->racks.empty() && state.rack_user_data.empty(), "bounded host ownership during churn");
        }
        ngs::release_system(state, mem, sys3);

        phase = "owned relocation and rollback";
        configured_module_count = 1;
        actual_parameter_bytes = 0;
        auto sys4 = system(mem, state);
        const auto initial_frees = mem.free_count;
        auto relocated = owned_rack(mem, state, sys4, 0x20000, 4096);
        check(relocated->voices.size() == 2 && relocated->voices[0].get(mem)->datas[0].info.size == 0,
            "zero-byte parameter module and multi-voice init");
        const auto owned_addr = state.rack_owned_blocks.at(relocated).address();
        ngs::release_system(state, mem, sys4);
        check(mem.free_count == initial_frees + 1 && !mem.owned_allocations.contains(owned_addr), "System shutdown frees owned block");
        check(state.rack_owned_blocks.empty(), "System shutdown clears ownership metadata");

        phase = "allocation and initialization rollback";
        auto sys5 = system(mem, state);
        auto def = mem.make<ngs::VoiceDefinition>();
        def->type = ngs::BussType::BUSS_SIMPLE;
        def->output_count = 2;
        SceNgsRackDescription description{};
        description.definition = Ptr<ngs::VoiceDefinition>(def, mem);
        description.voice_count = 1;
        description.channels_per_voice = 2;
        SceNgsBufferInfo info{ Ptr<void>(0x22000), 256 };
        Ptr<ngs::Rack> output;
        uint32_t size = 0;
        check(call_rack_size(env, sys5, &description, &size) == 0, "failure fixture size export query");
        info.size = size;
        mem.fail_next_allocation = true;
        output = Ptr<ngs::Rack>(0xDEAD);
        check(call_rack_init(env, sys5, &info, &description, &output) == SCE_NGS_ERROR, "backing allocation failure rejected");
        check(output.address() == 0xDEAD, "failed init leaves output handle unchanged");
        check(sys5->racks.empty() && state.rack_owned_blocks.empty(), "allocation failure publishes no rack");

        info.size = size - 1;
        const auto before_small = mem.free_count;
        check(!ngs::init_rack(state, mem, sys5, &info, &description, output), "undersized caller declaration rejected");
        check(mem.free_count == before_small, "undersized declaration allocates nothing");

        info.size = size;
        definition_calls = 0;
        throw_on_definition_call = 2;
        const auto before_definition = mem.free_count;
        check(!ngs::init_rack(state, mem, sys5, &info, &description, output), "definition failure rejected");
        throw_on_definition_call = 0;
        check(mem.free_count == before_definition + 1, "definition failure frees backing");
        check(sys5->racks.empty(), "definition failure leaves no rack");

        phase = "parameter stage rollback";
        configured_module_count = 2;
        actual_parameter_bytes = 7; // Odd sizes exercise the allocator's four-byte padding.
        description.voice_count = 2;
        info.size = 256;
        check(call_rack_size(env, sys5, &description, &size) == 0, "rollback size export query");
        info.size = size;
        initialization_calls = 0;
        throw_on_initialization_call = 2;
        const auto cleanup_before_failure = cleaned;
        const auto frees_before_failure = mem.free_count;
        check(!ngs::init_rack(state, mem, sys5, &info, &description, output), "parameter allocation failure rejected");
        throw_on_initialization_call = 0;
        check(cleaned > cleanup_before_failure && mem.free_count == frees_before_failure + 1,
            "partial voice cleanup and backing rollback");
        check(sys5->racks.empty() && state.rack_owned_blocks.empty() && state.rack_user_data.empty(), "failed init leaves no published state");
        configured_module_count = 1;
        actual_parameter_bytes = 0;
        ngs::release_system(state, mem, sys5);

        phase = "owned asynchronous callback lifetime";
        auto sys6 = system(mem, state);
        auto owned_d = owned_rack(mem, state, sys6, 0x24000, 4096);
        auto owned_e = owned_rack(mem, state, sys6, 0x26000, 4096);
        const Address owned_dh = Ptr<void>(owned_d, mem).address(), owned_eh = Ptr<void>(owned_e, mem).address();
        const Address dbacking = state.rack_owned_blocks.at(owned_d).address();
        const Address ebacking = state.rack_owned_blocks.at(owned_e).address();
        Env async_env{{}, state, mem};
        KernelState async_kernel;
        int async_callbacks = 0;
        async_kernel.thread->callback = [&](Address function, Address argument) {
            check(mem.owned_allocations.contains(async_callbacks == 0 ? dbacking : ebacking), "backing remains until callback returns");
            const auto &info = *Ptr<SceNgsCallbackInfo>(argument).get(mem);
            check(info.rack_handle.address() == (async_callbacks == 0 ? owned_dh : owned_eh), "callback keeps original opaque handle value");
            ++async_callbacks;
            if (async_callbacks == 1)
                check(call_rack_release(async_env, owned_e, Ptr<void>(0x201)) == SCE_NGS_OK, "reentrant next release accepted");
            (void)function;
        };
        check(call_rack_release(async_env, owned_d, Ptr<void>(0x200)) == SCE_NGS_OK, "owned async release accepted");
        sys6->voice_scheduler.update(async_kernel, mem, 1);
        check(async_callbacks == 2 && !mem.owned_allocations.contains(dbacking) && !mem.owned_allocations.contains(ebacking),
            "owned blocks freed after callback return");
        ngs::release_system(state, mem, sys6);

        phase = "size overflow and churn";
        auto overflow_def = mem.make<ngs::VoiceDefinition>();
        overflow_def->output_count = UINT32_MAX;
        description.definition = Ptr<ngs::VoiceDefinition>(overflow_def, mem);
        description.voice_count = INT32_MAX;
        description.patches_per_output = INT32_MAX;
        auto sys7 = system(mem, state);
        check(call_rack_size(env, sys7, &description, &size) == SCE_NGS_ERROR_INVALID_ARG, "overflow size rejected by export");
        const auto repeated_frees = mem.free_count;
        for (int i = 0; i < 24; ++i) {
            auto r = owned_rack(mem, state, sys7, 0x28000, 4096);
            const auto addr = state.rack_owned_blocks.at(r).address();
            const auto block = ngs::release_rack(state, mem, sys7, r);
            check(block.address() == addr && mem.owned_allocations.contains(addr), "sync release returns owned block before caller frees");
            ::free(mem, block.address());
        }
        check(mem.free_count == repeated_frees + 24 && sys7->racks.empty(), "repeated create/release has no block leak");
        ngs::release_system(state, mem, sys7);
        std::puts("NGS fixture PASS: relocation handle/canary, alignment/zero-size, overflow, rollback, async callback lifetime, System shutdown, churn, routes");
        check(run_ngs_review_fixture() == 0, "real-module rollback and poisoned reuse fixture");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL at %s: %s\n", phase, error.what());
        return 1;
    }
}
// Included after the integration fixture's adapters, with a real module factory
// for BUSS_MASTER and BUSS_MIXER. Called sources are not reimplemented here.
#include <cstdlib>
#include <new>
static int fail_after = -1;
static bool injected = false;
void *operator new(std::size_t size) {
    if (fail_after >= 0 && fail_after-- == 0) {
        fail_after = -1;
        injected = true;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

static SceNgsRackDescription description(MemState &mem, ngs::BussType type,
    int voices, int outputs, int patches) {
    auto def = mem.make<ngs::VoiceDefinition>();
    def->type = type;
    def->output_count = outputs;
    SceNgsRackDescription result{};
    result.definition = Ptr<ngs::VoiceDefinition>(def, mem);
    result.voice_count = voices;
    result.channels_per_voice = 2;
    result.max_patches_per_input = 1;
    result.patches_per_output = patches;
    return result;
}
static ngs::Rack *create(Env &env, ngs::System *sys, SceNgsRackDescription &desc) {
    uint32_t required = 0;
    check(call_rack_size(env, sys, &desc, &required) == 0, "size query");
    SceNgsBufferInfo info{Ptr<void>(0x20000), required};
    Ptr<ngs::Rack> handle;
    check(call_rack_init(env, sys, &info, &desc, &handle) == SCE_NGS_OK, "zero-size/real module init");
    check(handle.address() != info.data.address(), "real relocated export handle");
    return handle.get(env.mem);
}
static void zero_and_alignment() {
    int cases = 0;
    for (auto type : {ngs::BussType::BUSS_MIXER, ngs::BussType::BUSS_MASTER, ngs::BussType::BUSS_SIMPLE}) {
        for (int voices : {1, 2, 3}) for (int patches : {0, 1, 3}) {
            MemState mem;
            ngs::State state;
            auto sys = system(mem, state);
            Env env{{}, state, mem};
            actual_parameter_bytes = type == ngs::BussType::BUSS_SIMPLE ? 7 : 0;
            auto desc = description(mem, type, voices, 1, patches);
            auto r = create(env, sys, desc);
            for (auto vh : r->voices) {
                check(vh.address() % alignof(ngs::Voice) == 0, "EVERY voice is aligned");
                auto v = vh.get(mem);
                if (type == ngs::BussType::BUSS_MASTER)
                    check(v->datas[1].guest_state_data.size() == 64 * 2 * 2, "real OutputModule initializes without parameter storage");
            }
            ngs::release_system(state, mem, sys);
            check(mem.owned_allocations.empty() && state.rack_owned_blocks.empty(), "all backing reclaimed");
            ++cases;
        }
    }
    std::printf("PASS real-module/zero/odd-padding alignment matrix: %d cases\n", cases);
}
static void allocation_failures() {
    int faults = 0;
    for (int nth = 0; nth < 100; ++nth) {
        MemState mem;
        ngs::State state;
        auto sys = system(mem, state);
        Env env{{}, state, mem};
        auto desc = description(mem, ngs::BussType::BUSS_MASTER, 3, 1, 0);
        uint32_t required = 0;
        check(call_rack_size(env, sys, &desc, &required) == 0, "failure setup query");
        SceNgsBufferInfo info{Ptr<void>(0x20000), required};
        Ptr<ngs::Rack> handle(0xdead);
        injected = false;
        fail_after = nth;
        auto rc = call_rack_init(env, sys, &info, &desc, &handle);
        fail_after = -1;
        if (injected) {
            ++faults;
            check(rc != SCE_NGS_OK && handle.address() == 0xdead, "failed allocation never publishes output");
            check(sys->racks.empty() && state.rack_user_data.empty() && state.rack_owned_blocks.empty()
                && mem.owned_allocations.empty(), "rollback after EVERY host allocation fault");
        } else {
            check(rc == SCE_NGS_OK, "first non-fault init succeeds");
        }
        ngs::release_system(state, mem, sys);
        if (!injected) break;
    }
    check(faults > 20, "fault sweep reaches later voices, output state and metadata publication");
    std::printf("PASS host-allocation rollback sweep: %d fault sites\n", faults);
}
static void routes_and_reuse() {
    MemState mem;
    ngs::State state;
    auto sys = system(mem, state);
    Env env{{}, state, mem};
    actual_parameter_bytes = 7;
    auto desc = description(mem, ngs::BussType::BUSS_SIMPLE, 3, 2, 2);
    auto source = create(env, sys, desc), dest = create(env, sys, desc);
    auto old_address = Ptr<ngs::Rack>(dest, mem).address();
    std::vector<Ptr<ngs::Patch>> routes;
    for (auto voice : source->voices) for (int port = 0; port < 2; ++port) for (int slot = 0; slot < 2; ++slot) {
        SceNgsPatchSetupInfo setup{voice, port, slot, dest->voices[slot], 0};
        auto p = sys->voice_scheduler.patch(mem, &setup);
        check(bool(p) && p.address() % alignof(ngs::Patch) == 0, "production patch allocation fits queried pool");
        routes.push_back(p);
    }
    KernelState kernel;
    ngs::Rack *during_callback = nullptr;
    kernel.thread->callback = [&](Address, Address info_address) {
        check(mem.owned_allocations.contains(old_address), "old block retained during completion");
        check(Ptr<SceNgsCallbackInfo>(info_address).get(mem)->rack_handle.address() == old_address, "callback snapshots relocated handle");
        for (auto p : routes)
            check(!p.get(mem)->is_active() && !p.get(mem)->dest && !p.get(mem)->source, "live source retains no retired destination");
        check(call_rack_release(env, dest, Ptr<void>()) == SCE_NGS_ERROR_INVALID_HANDLE, "callback cannot dereference retired rack");
        during_callback = create(env, sys, desc);
        check(Ptr<ngs::Rack>(during_callback, mem).address() != old_address, "no reuse before callback returns");
    };
    check(call_rack_release(env, dest, Ptr<void>(0x100)) == SCE_NGS_OK, "enqueue owned release");
    sys->voice_scheduler.update(kernel, mem, 1);
    check(!mem.owned_allocations.contains(old_address), "free after completion");
    auto after = create(env, sys, desc);
    check(Ptr<ngs::Rack>(after, mem).address() == old_address, "freed block reused AFTER completion in adversarial allocator");
    for (auto p : routes) check(!p.get(mem)->dest, "no stale route after address reuse");
    ngs::release_system(state, mem, sys);
    check(mem.owned_allocations.empty(), "reuse scenario leaves no owned block");
    std::puts("PASS production patch allocations, cross-rack detach, callback reentry and poisoned block reuse");
}
int run_ngs_review_fixture() {
    try {
        zero_and_alignment();
        allocation_failures();
        routes_and_reuse();
    } catch (const std::exception &e) {
        fail_after = -1;
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    return 0;
}
