// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <limits>

namespace codec {
// VitaSDK audiodec.h: AAC permits exactly one frame per DecodeNFrames call.
constexpr bool valid_aac_frame_count(uint32_t frames) {
    return frames == 1;
}

// A declared PCM limit is an API bound, not proof of the guest allocation size.
constexpr bool aac_pcm_bytes(uint32_t samples, uint32_t channels, bool sbr,
    uint32_t capacity, uint32_t &bytes) {
    bytes = 0;
    if (channels < 1 || channels > 2 || samples > (sbr ? 2048u : 1024u))
        return false;
    const uint64_t count = uint64_t(samples) * channels * sizeof(int16_t);
    if (count > capacity || count > std::numeric_limits<uint32_t>::max())
        return false;
    bytes = static_cast<uint32_t>(count);
    return true;
}
} // namespace codec
