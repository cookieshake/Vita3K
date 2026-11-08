// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cpu/state.h>
#include <kernel/state.h>

#include <ngs/modules/atrac9.h>
#include <ngs/state.h>
#include <ngs/system.h>
#include <util/lock_and_find.h>

#include <util/vector_utils.h>

#include <limits>

namespace ngs {
Rack::Rack(System *mama, const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , system(mama) {}

System::System(const Ptr<void> memspace, const uint32_t memspace_size)
    : MempoolObject(memspace, memspace_size)
    , max_voices(0)
    , granularity(0)
    , sample_rate(0) {}

bool Patch::is_active() const {
    return output_sub_index != -1;
}

void VoiceInputManager::init(const uint32_t granularity, const uint16_t total_input) {
    inputs.resize(total_input);

    for (auto &input : inputs) {
        // FLTP and maximum channel count
        input.resize(granularity * 8);
    }

    reset_inputs();
}

void VoiceInputManager::reset_inputs() {
    for (auto &input : inputs) {
        std::ranges::fill(input, 0);
    }
}

VoiceInputManager::PCMInput *VoiceInputManager::get_input_buffer_queue(const int32_t index) {
    if (index >= inputs.size()) {
        return nullptr;
    }

    return &inputs[index];
}

int32_t VoiceInputManager::receive(const MemState &mem, ngs::Patch *patch, const VoiceProduct &product) {
    Voice *source = patch->source.get(mem);
    Voice *dest = patch->dest.get(mem);
    if (!source || !dest) {
        return -1;
    }

    PCMInput *input = get_input_buffer_queue(patch->dest_index);

    if (!input) {
        return -1;
    }

    float *dest_buffer = reinterpret_cast<float *>(input->data());
    const float *data_to_mix_in = reinterpret_cast<const float *>(product.data);

    float volume_matrix[2][2];
    memcpy(volume_matrix, patch->volume_matrix, sizeof(volume_matrix));

    // we always use stereo internally, so make sure not to add too many channels
    if (source->rack->channels_per_voice == 1) {
        volume_matrix[1][0] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    if (dest->rack->channels_per_voice == 1) {
        volume_matrix[0][1] = 0.0f;
        volume_matrix[1][1] = 0.0f;
    }

    // Try mixing, also with the use of this volume matrix
    // Dest is our voice to receive this data.
    for (int32_t k = 0; k < dest->rack->system->granularity; k++) {
        dest_buffer[k * 2] = std::clamp(dest_buffer[k * 2] + data_to_mix_in[k * 2] * volume_matrix[0][0]
                + data_to_mix_in[k * 2 + 1] * volume_matrix[1][0],
            -1.0f, 1.0f);
        dest_buffer[k * 2 + 1] = std::clamp(dest_buffer[k * 2 + 1] + data_to_mix_in[k * 2] * volume_matrix[0][1] + data_to_mix_in[k * 2 + 1] * volume_matrix[1][1], -1.0f, 1.0f);
    }

    return 0;
}

ModuleData::ModuleData()
    : callback(0)
    , user_data(0)
    , is_bypassed(false)
    , flags(0) {
}

SceNgsBufferInfo *ModuleData::lock_params(const MemState &mem) {
    const std::lock_guard<std::mutex> guard(*parent->voice_mutex);

    // Save a copy of previous set of data
    if (flags & PARAMS_LOCK) {
        return nullptr;
    }

    const uint8_t *current_data = info.data.cast<const uint8_t>().get(mem);
    last_info.resize(info.size);
    memcpy(last_info.data(), current_data, info.size);

    flags |= PARAMS_LOCK;

    return &info;
}

bool ModuleData::unlock_params(const MemState &mem) {
    const std::lock_guard<std::mutex> guard(*parent->voice_mutex);

    parent->rack->modules[index]->on_param_change(mem, *this);

    if (flags & PARAMS_LOCK) {
        flags &= ~PARAMS_LOCK;
        return true;
    }

    return false;
}

void ModuleData::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, const uint32_t reason1,
    const uint32_t reason2, Address reason_ptr) {
    return parent->invoke_callback(kernel, mem, thread_id, callback, user_data, parent->rack->modules[index]->module_id(),
        reason1, reason2, reason_ptr);
}

void Voice::init(Rack *mama) {
    rack = mama;
    state = VoiceState::VOICE_STATE_AVAILABLE;
    is_pending = false;
    is_paused = false;
    is_keyed_off = false;

    datas.resize(mama->modules.size());

    for (uint32_t i = 0; i < MAX_OUTPUT_PORT; i++)
        patches[i].resize(mama->patches_per_output);

    inputs.init(rack->system->granularity, 1);
    voice_mutex = std::make_unique<std::mutex>();
}

Ptr<Patch> Voice::patch(const MemState &mem, const int32_t index, int32_t subindex, int32_t dest_index, Ptr<Voice> source, Ptr<Voice> dest) {
    const std::lock_guard<std::mutex> guard(*voice_mutex);

    if (index >= MAX_OUTPUT_PORT) {
        // We don't have enough port for you!
        return {};
    }

    // Look if another patch has already been there
    if (subindex == -1) {
        for (int32_t i = 0; i < patches[index].size(); i++) {
            if (!patches[index][i] || (patches[index])[i].get(mem)->output_sub_index == -1) {
                subindex = i;
                break;
            }
        }
    }

    if (subindex >= patches[index].size()) {
        return {};
    }

    if (patches[index][subindex] && patches[index][subindex].get(mem)->output_sub_index != -1) {
        // You just hit an occupied subindex! You won't get to eat, stay in detention.
        return {};
    }

    if (!patches[index][subindex]) {
        // Create the patch incase it's not yet existed
        patches[index][subindex] = rack->alloc_and_init<Patch>(mem);
    }

    Patch *patch = patches[index][subindex].get(mem);

    patch->output_sub_index = subindex;
    patch->output_index = index;
    patch->dest_index = dest_index;
    patch->dest = dest;
    patch->source = source;

    // Initialize the matrix
    memset(patch->volume_matrix, 0, sizeof(patch->volume_matrix));

    return patches[index][subindex];
}

bool Voice::remove_patch(const MemState &mem, const Ptr<Patch> patch) {
    if (!patch || !voice_mutex) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(*voice_mutex);
    bool found = false;
    for (auto &patches_1 : patches) {
        if (std::ranges::contains(patches_1, patch)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    // Try to unroute. Free the destination index
    patch.get(mem)->output_sub_index = -1;

    return true;
}

ModuleData *Voice::module_storage(const uint32_t index) {
    if (index >= datas.size()) {
        return nullptr;
    }

    return &datas[index];
}

void Voice::transition(const MemState &mem, const VoiceState new_state) {
    const VoiceState old = state;
    state = new_state;

    for (size_t i = 0; i < datas.size(); i++) {
        rack->modules[i]->on_state_change(mem, datas[i], old);
    }
}

bool Voice::parse_params(const MemState &mem, const SceNgsModuleParamHeader *header) {
    ModuleData *storage = module_storage(header->module_id);

    if (!storage)
        return false;

    if (storage->flags & ModuleData::PARAMS_LOCK)
        return false;

    const auto *descr = reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1);
    if (descr->size > storage->info.size)
        return false;

    memcpy(storage->info.data.get(mem), descr, descr->size);

    return true;
}

SceInt32 Voice::parse_params_block(const MemState &mem, const SceNgsModuleParamHeader *header, const SceUInt32 size) {
    const SceUInt8 *data = reinterpret_cast<const SceUInt8 *>(header);
    const SceUInt8 *data_end = data + size;

    SceInt32 num_error = 0;

    // after first loop, check if other module exist
    while (data < data_end) {
        if (!parse_params(mem, header))
            num_error++;

        // increment by the size of the header alone + the descriptor size
        data += sizeof(SceNgsModuleParamHeader) + reinterpret_cast<const SceNgsParamsDescriptor *>(header + 1)->size;

        // set new header for next module
        header = reinterpret_cast<const SceNgsModuleParamHeader *>(data);
    }

    return num_error;
}

bool Voice::set_preset(const MemState &mem, const SceNgsVoicePreset *preset) {
    // we ignore the name for now
    const uint8_t *data_origin = reinterpret_cast<const uint8_t *>(preset);

    if (preset->preset_data_offset) {
        const auto *preset_data = reinterpret_cast<const SceNgsModuleParamHeader *>(data_origin + preset->preset_data_offset);
        auto nb_errors = parse_params_block(mem, preset_data, preset->preset_data_size);
        if (nb_errors > 0)
            return false;
    }

    if (preset->bypass_flags_offset) {
        const auto *bypass_flags = reinterpret_cast<const SceUInt32 *>(data_origin + preset->bypass_flags_offset);
        // should we disable bypass on all modules first?
        for (SceUInt32 i = 0; i < preset->bypass_flags_nb; i++) {
            ModuleData *module_data = module_storage(*bypass_flags);
            if (!module_data)
                return false;
            module_data->is_bypassed = true;

            bypass_flags++;
        }
    }

    return true;
}

void Voice::invoke_callback(KernelState &kernel, const MemState &mem, const SceUID thread_id, Ptr<void> callback, Ptr<void> user_data,
    const uint32_t module_id, const uint32_t reason1, const uint32_t reason2, Address reason_ptr) {
    if (!callback) {
        return;
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    const Address callback_info_addr = thread->cpu->stack_alloc(sizeof(SceNgsCallbackInfo));

    SceNgsCallbackInfo *info = Ptr<SceNgsCallbackInfo>(callback_info_addr).get(mem);
    info->rack_handle = Ptr<void>(rack, mem);
    info->voice_handle = Ptr<void>(this, mem);
    info->module_id = module_id;
    info->callback_reason = reason1;
    info->callback_reason_2 = reason2;
    info->callback_ptr = Ptr<void>(reason_ptr);
    info->userdata = user_data;

    thread->call_guest(callback.address(), { callback_info_addr });
    thread->cpu->stack_free(sizeof(SceNgsCallbackInfo));
}

uint32_t System::get_required_memspace_size(SceNgsSystemInitParams *parameters) {
    return sizeof(System);
}

bool Rack::get_required_memspace_size(MemState &mem, const SceNgsRackDescription *description, uint32_t &size) {
    if (!description || !description->definition || description->voice_count <= 0 || description->channels_per_voice < 0
        || description->channels_per_voice > 2 || description->max_patches_per_input < 0 || description->patches_per_output < 0)
        return false;

    VoiceDefinition *definition = description->definition.get(mem);
    if (!definition || definition->output_count == 0 || definition->type >= BussType::BUSS_MAX)
        return false;

    // Report host layout size with checked arithmetic for untrusted counts.
    constexpr uint64_t max_size = std::numeric_limits<uint64_t>::max();
    std::vector<std::unique_ptr<Module>> modules;
    try {
        apply_voice_definition(definition, modules);
    } catch (...) {
        return false;
    }
    uint64_t parameters_per_voice = 0;
    for (const auto &module : modules) {
        const uint64_t parameter_size = module->get_buffer_parameter_size();
        const uint64_t aligned_parameter_size = (parameter_size + 3u) & ~uint64_t(3u);
        if (aligned_parameter_size > (max_size - parameters_per_voice) / 2)
            return false;
        parameters_per_voice += 2 * aligned_parameter_size;
    }
    uint64_t patches_per_voice = static_cast<uint32_t>(description->patches_per_output);
    if (definition->output_count && patches_per_voice > max_size / definition->output_count)
        return false;
    patches_per_voice *= definition->output_count;
    if (sizeof(Patch) && patches_per_voice > max_size / sizeof(Patch))
        return false;
    patches_per_voice *= sizeof(Patch);
    if (parameters_per_voice > max_size - sizeof(Voice)
        || patches_per_voice > max_size - sizeof(Voice) - parameters_per_voice)
        return false;
    const uint64_t per_voice = sizeof(Voice) + parameters_per_voice + patches_per_voice;
    if (per_voice && static_cast<uint64_t>(description->voice_count) > (max_size - sizeof(Rack)) / per_voice)
        return false;
    const uint64_t total = sizeof(Rack) + static_cast<uint64_t>(description->voice_count) * per_voice;
    if (total > std::numeric_limits<uint32_t>::max())
        return false;

    size = static_cast<uint32_t>(total);
    return true;
}

bool init(State &ngs, MemState &mem) {
    voice_definition_init(ngs, mem);

    return true;
}

void deinit(State &ngs, MemState &mem) {
    while (!ngs.systems.empty()) {
        release_system(ngs, mem, ngs.systems.back());
    }

    Atrac9Module::free_swr_contexts();

    ngs.definitions = Ptr<VoiceDefinition>(0);
}

bool init_system(State &ngs, const MemState &mem, SceNgsSystemInitParams *parameters, Ptr<void> memspace, const uint32_t memspace_size) {
    // Reserve first memory allocation for our System struct
    System *sys = memspace.cast<System>().get(mem);
    sys = new (sys) System(memspace, memspace_size);

    sys->racks.resize(parameters->max_racks);

    sys->max_voices = parameters->max_voices;
    sys->granularity = parameters->granularity;
    sys->sample_rate = parameters->sample_rate;

    // Alloc first block for System struct
    if (!sys->alloc_raw(sizeof(System))) {
        return false;
    }

    ngs.systems.push_back(sys);
    return true;
}

void release_system(State &ngs, MemState &mem, System *system) {
    // System shutdown uses the same route detachment and module cleanup as
    // individual rack release. Caller guarantees no update/callback is active.
    while (true) {
        const auto rack = std::find_if(system->racks.begin(), system->racks.end(),
            [](Rack *entry) { return entry != nullptr; });
        if (rack == system->racks.end())
            break;
        const Ptr<void> backing = release_rack(ngs, mem, system, *rack);
        if (backing)
            ::free(mem, backing.address());
    }

    system->racks.clear();

    vector_utils::erase_first(ngs.systems, system);
    system->~System();
}

bool init_rack(State &ngs, MemState &mem, System *system, SceNgsBufferInfo *init_info, const SceNgsRackDescription *description,
    Ptr<Rack> &rack_handle) {
    uint32_t required_size = 0;
    if (!system || !init_info || !init_info->data || !init_info->size
        || !Rack::get_required_memspace_size(mem, description, required_size)
        || init_info->size < required_size
        // mem::alloc rounds to a 4 KiB page; keep its alignment addition in range.
        || required_size > std::numeric_limits<uint32_t>::max() - 4095u) {
        return false;
    }

    const Address backing_address = ::alloc(mem, required_size, "NGS Rack backing");
    if (!backing_address)
        return false;
    const Ptr<void> backing(backing_address);
    Rack *rack = nullptr;
    bool rack_constructed = false;
    size_t completed_voices = 0;
    size_t initialized_modules = 0;
    Voice *partial_voice = nullptr;

    auto rollback = [&] {
        if (rack_constructed) {
            for (size_t voice_index = 0; voice_index < completed_voices; ++voice_index) {
                Voice *voice = rack->voices[voice_index].get(mem);
                for (size_t module_index = 0; module_index < rack->modules.size(); ++module_index) {
                    if (rack->modules[module_index])
                        rack->modules[module_index]->cleanup_voice_state(voice->datas[module_index]);
                }
                voice->~Voice();
            }
            if (partial_voice) {
                for (size_t module_index = 0; module_index < initialized_modules; ++module_index) {
                    if (rack->modules[module_index])
                        rack->modules[module_index]->cleanup_voice_state(partial_voice->datas[module_index]);
                }
                partial_voice->~Voice();
            }
            rack->~Rack();
        }
        ::free(mem, backing.address());
    };

    try {
        rack = new (backing.cast<Rack>().get(mem)) Rack(system, backing, required_size);
        rack_constructed = true;
        if (!rack->alloc<Rack>()) {
            rollback();
            return false;
        }

        apply_voice_definition(description->definition.get(mem), rack->modules);
        rack->channels_per_voice = description->channels_per_voice;
        rack->max_patches_per_input = description->max_patches_per_input;
        rack->patches_per_output = description->patches_per_output;
        rack->voices.resize(description->voice_count);
        rack->vdef = description->definition.get(mem);

        for (auto &voice_handle : rack->voices) {
            voice_handle = rack->alloc<Voice>();
            if (!voice_handle) {
                rollback();
                return false;
            }

            partial_voice = voice_handle.get(mem);
            new (partial_voice) Voice();
            partial_voice->init(rack);
            initialized_modules = 0;
            for (size_t module_index = 0; module_index < rack->modules.size(); ++module_index) {
                ModuleData &data = partial_voice->datas[module_index];
                data.info.size = rack->modules[module_index]->get_buffer_parameter_size();
                data.info.data.reset();
                // Zero-byte interfaces need initialization, but no pool storage.
                // alloc_raw(0) can fail after the last Voice exactly fills the pool.
                if (data.info.size != 0) {
                    data.info.data = rack->alloc_raw(data.info.size);
                    // Keep the second VoiceLock parameter copy inside the owned block.
                    const Ptr<void> second_copy = rack->alloc_raw(data.info.size);
                    if (!data.info.data || !second_copy) {
                        rollback();
                        return false;
                    }
                }
                data.parent = partial_voice;
                data.index = static_cast<uint32_t>(module_index);
                ++initialized_modules;
                rack->modules[module_index]->initialize_voice_data(data);
            }
            ++completed_voices;
            partial_voice = nullptr;
            initialized_modules = 0;
        }

        system->racks.push_back(rack);
        bool metadata_committed = false;
        try {
            {
                const std::lock_guard<std::mutex> lock(ngs.rack_metadata_mutex);
                const auto [owned_it, owned_inserted] = ngs.rack_owned_blocks.emplace(rack, backing);
                if (owned_inserted) {
                    try {
                        const auto [user_it, user_inserted] = ngs.rack_user_data.emplace(rack, description->unk14);
                        (void)user_it;
                        if (user_inserted) {
                            metadata_committed = true;
                        } else {
                            ngs.rack_owned_blocks.erase(owned_it);
                        }
                    } catch (...) {
                        ngs.rack_owned_blocks.erase(owned_it);
                        throw;
                    }
                }
            }
        } catch (...) {
            system->racks.pop_back();
            rollback();
            return false;
        }
        if (!metadata_committed) {
            system->racks.pop_back();
            rollback();
            return false;
        }

        rack_handle = Ptr<Rack>(rack, mem);
        return true;
    } catch (...) {
        rollback();
        return false;
    }
}

Ptr<VoiceDefinition> get_voice_definition(State &ngs, MemState &mem, ngs::BussType type) {
    return ngs.definitions + static_cast<int>(type);
}
} // namespace ngs
