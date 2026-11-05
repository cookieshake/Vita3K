// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ngs/state.h>
#include <ngs/system.h>

namespace ngs {
Ptr<void> release_rack(State &ngs, MemState &mem, System *system, Rack *rack) {
    // Only call at an update safe point, under the scheduler mutex. Removing a
    // voice from the live queue does not remove it from update's queue_copy.
    if (!rack || std::find(system->racks.begin(), system->racks.end(), rack) == system->racks.end())
        return {};

    // A patch is owned by its SOURCE rack. Walk all sources before destroying
    // any voice; inbound patches otherwise retain pointers into the dead rack.
    for (Rack *source_rack : system->racks) {
        if (!source_rack)
            continue;
        for (const auto source_handle : source_rack->voices) {
            for (auto &output : source_handle.get(mem)->patches) {
                for (const auto patch_handle : output) {
                    if (!patch_handle)
                        continue;
                    Patch *patch = patch_handle.get(mem);
                    if (source_rack == rack
                        || std::find(rack->voices.begin(), rack->voices.end(), patch->dest) != rack->voices.end()) {
                        patch->output_sub_index = -1;
                        patch->source.reset();
                        patch->dest.reset();
                    }
                }
            }
        }
    }

    for (const auto voice : rack->voices)
        std::erase(system->voice_scheduler.queue, voice.get(mem));

    for (const auto voice : rack->voices) {
        Voice *v = voice.get(mem);
        for (size_t i = 0; i < rack->modules.size() && i < v->datas.size(); ++i) {
            if (rack->modules[i])
                rack->modules[i]->cleanup_voice_state(v->datas[i]);
        }
        v->~Voice();
    }

    std::erase(system->racks, rack);
    Ptr<void> owned_backing;
    {
        const std::lock_guard<std::mutex> lock(ngs.rack_metadata_mutex);
        ngs.rack_user_data.erase(rack);
        const auto owned = ngs.rack_owned_blocks.find(rack);
        if (owned != ngs.rack_owned_blocks.end()) {
            owned_backing = owned->second;
            ngs.rack_owned_blocks.erase(owned);
        }
    }
    rack->~Rack();
    // The caller frees this only after any completion callback has returned.
    return owned_backing;
}
} // namespace ngs
