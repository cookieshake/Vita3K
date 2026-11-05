// Test-only host adapter for exercising NGS core entry points without the
// emulator's full Env/module runtime. Production module dispatch is not linked.
#pragma once

#include <ngs/state.h>
#include <ngs/system.h>

constexpr int SCE_NGS_OK = 0;
constexpr int SCE_NGS_ERROR = 0x804A0001;
constexpr int SCE_NGS_ERROR_INVALID_ARG = 0x804A0002;
constexpr int SCE_NGS_ERROR_RESOURCE_LOCKED = 0x804A0008;
constexpr int SCE_NGS_ERROR_INVALID_STATE = 0x804A0010;
constexpr int SCE_NGS_ERROR_INVALID_HANDLE = 0x804A000C;
constexpr int SCE_NGS_ERROR_PATCH_NOT_AVAIL = 0x804A000E;

static int call_rack_size(Env &env, ngs::System *, SceNgsRackDescription *description, uint32_t *size) {
    if (!description || !size)
        return SCE_NGS_ERROR_INVALID_ARG;
    return ngs::Rack::get_required_memspace_size(env.mem, description, *size) ? 0 : SCE_NGS_ERROR_INVALID_ARG;
}

static int call_rack_init(Env &env, ngs::System *system, SceNgsBufferInfo *info,
    const SceNgsRackDescription *description, Ptr<ngs::Rack> *rack) {
    if (!info || !system || !description || !description->definition || !rack)
        return SCE_NGS_ERROR_INVALID_ARG;
    Ptr<ngs::Rack> relocated;
    if (!ngs::init_rack(env.ngs, env.mem, system, info, description, relocated))
        return SCE_NGS_ERROR;
    *rack = relocated;
    return SCE_NGS_OK;
}

static int call_system_release(Env &env, ngs::System *system) {
    if (!system)
        return SCE_NGS_ERROR_INVALID_ARG;
    auto &scheduler = system->voice_scheduler;
    {
        std::unique_lock<std::recursive_mutex> lock(scheduler.mutex);
        if (scheduler.is_update_thread())
            return SCE_NGS_ERROR_RESOURCE_LOCKED;
        scheduler.condvar.wait(lock, [&] { return !scheduler.is_updating; });
        if (!scheduler.operations_pending.empty())
            return SCE_NGS_ERROR_RESOURCE_LOCKED;
    }
    ngs::release_system(env.ngs, env.mem, system);
    return SCE_NGS_OK;
}

static int call_rack_release(Env &env, ngs::Rack *rack, Ptr<void> callback) {
    ngs::System *system = nullptr;
    {
        const std::lock_guard<std::mutex> lock(env.ngs.rack_metadata_mutex);
        if (!rack || !env.ngs.rack_user_data.contains(rack))
            return SCE_NGS_ERROR_INVALID_HANDLE;
        system = rack->system;
    }
    auto &scheduler = system->voice_scheduler;
    std::unique_lock<std::recursive_mutex> lock(scheduler.mutex);
    {
        const std::lock_guard<std::mutex> metadata_lock(env.ngs.rack_metadata_mutex);
        if (!env.ngs.rack_user_data.contains(rack))
            return SCE_NGS_ERROR_INVALID_HANDLE;
    }
    auto pending = scheduler.operations_pending;
    while (!pending.empty()) {
        if (pending.front().release_data.rack == rack)
            return SCE_NGS_ERROR_INVALID_STATE;
        pending.pop();
    }
    if (!callback && !scheduler.is_updating) {
        const Ptr<void> backing = ngs::release_rack(env.ngs, env.mem, system, rack);
        if (backing)
            ::free(env.mem, backing.address());
    } else if (!callback) {
        if (scheduler.is_update_thread())
            return SCE_NGS_ERROR_RESOURCE_LOCKED;
        scheduler.condvar.wait(lock, [&] { return !scheduler.is_updating; });
        {
            const std::lock_guard<std::mutex> metadata_lock(env.ngs.rack_metadata_mutex);
            if (!env.ngs.rack_user_data.contains(rack))
                return SCE_NGS_ERROR_INVALID_HANDLE;
        }
        const Ptr<void> backing = ngs::release_rack(env.ngs, env.mem, system, rack);
        if (backing)
            ::free(env.mem, backing.address());
    } else {
        ngs::OperationPending op;
        op.type = ngs::PendingType::ReleaseRack;
        op.system = system;
        op.release_data.state = &env.ngs;
        op.release_data.rack = rack;
        op.release_data.callback = callback.address();
        {
            const std::lock_guard<std::mutex> metadata_lock(env.ngs.rack_metadata_mutex);
            op.release_data.user_data = env.ngs.rack_user_data.at(rack).address();
        }
        scheduler.operations_pending.push(op);
    }
    return SCE_NGS_OK;
}
