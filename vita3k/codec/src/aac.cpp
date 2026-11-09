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

#include <codec/state.h>
#include <codec/aac_output.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_internal.h>
#include <libswresample/swresample.h>
}

#include <util/log.h>

AacDecoderState::AacDecoderState(uint32_t sample_rate, uint32_t channels, bool sbr)
    : output_sample_rate(sample_rate)
    , output_channels(channels)
    , is_sbr(sbr) {
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!codec || !sample_rate || sample_rate > INT_MAX || channels < 1 || channels > 2)
        return;
    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    resampler_format = av_frame_alloc();
    if (!context || !frame || !resampler_format)
        return;
    context->codec_type = AVMEDIA_TYPE_AUDIO;
    av_channel_layout_default(&context->ch_layout, channels);
    context->sample_rate = sample_rate;
    if (avcodec_open2(context, codec, nullptr) < 0)
        LOG_WARN("Failed to open AAC decoder.");
}

AacDecoderState::~AacDecoderState() {
    av_frame_free(&resampler_format);
    av_frame_free(&frame);
    swr_free(&swr);
}

bool AacDecoderState::is_initialized() const {
    return context && frame && resampler_format && avcodec_is_open(context);
}

uint32_t AacDecoderState::get(DecoderQuery query) {
    switch (query) {
    case DecoderQuery::CHANNELS: return output_channels;
    case DecoderQuery::BIT_RATE: return context ? context->bit_rate : 0;
    case DecoderQuery::SAMPLE_RATE: return output_sample_rate;
    default: return 0;
    }
}

bool AacDecoderState::send(const uint8_t *data, uint32_t size) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    has_frame = false;
    output_ready = false;
    output_pcm.clear();
    es_size_used = 0;
    if (!context || !frame || !resampler_format || !avcodec_is_open(context)
        || !data || !size || size > INT_MAX)
        return false;

    av_frame_unref(frame);
    AVPacket *packet = av_packet_alloc();
    if (!packet)
        return false;
    // FFmpeg's decoder requires padding beyond the compressed packet. The guest
    // pointer does not promise it, so own a padded packet rather than borrowing it.
    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, data, size);
    int got_frame = 0;
    const int len = ffcodec(codec)->cb.decode(context, frame, &got_frame, packet);
    av_packet_free(&packet);
    if (len < 0 || !got_frame || static_cast<uint32_t>(len) > size) {
        av_frame_unref(frame);
        return false;
    }
    es_size_used = static_cast<uint32_t>(len);
    has_frame = true;
    return true;
}

bool AacDecoderState::prepare_output() {
    if (output_ready)
        return true;
    if (!has_frame || !frame || !resampler_format || frame->nb_samples <= 0
        || frame->nb_samples > (is_sbr ? 2048 : 1024)
        || frame->sample_rate <= 0 || !av_channel_layout_check(&frame->ch_layout)
        || frame->ch_layout.nb_channels > 2 || frame->format < 0)
        return false;

    // For SBR, FFmpeg already reports the expanded output rate. Do not blindly
    // double the request: it may already name that expanded rate.
    const uint32_t rate = is_sbr ? frame->sample_rate : output_sample_rate;
    if (!swr || resampler_format->sample_rate != frame->sample_rate
        || resampler_format->format != frame->format
        || av_channel_layout_compare(&resampler_format->ch_layout, &frame->ch_layout) != 0
        || output_sample_rate != rate) {
        swr_free(&swr);
        AVChannelLayout output_layout{};
        av_channel_layout_default(&output_layout, output_channels);
        int ret = swr_alloc_set_opts2(&swr, &output_layout, AV_SAMPLE_FMT_S16, rate,
            &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&output_layout);
        if (ret < 0 || !swr || swr_init(swr) < 0) {
            swr_free(&swr);
            return false;
        }
        av_frame_unref(resampler_format);
        if (av_channel_layout_copy(&resampler_format->ch_layout, &frame->ch_layout) < 0) {
            swr_free(&swr);
            return false;
        }
        resampler_format->sample_rate = frame->sample_rate;
        resampler_format->format = frame->format;
        output_sample_rate = rate;
    }

    const int bound = swr_get_out_samples(swr, frame->nb_samples);
    // A corrupt or unsupported rate ratio must not cause an unbounded native
    // allocation. This is a staging limit, never a guest destination capacity.
    const uint32_t max_samples = is_sbr ? 2048 : 1024;
    if (bound < 0 || static_cast<uint32_t>(bound) > max_samples * 4)
        return false;
    output_pcm.resize(static_cast<size_t>(bound) * output_channels * sizeof(int16_t));
    uint8_t *dest = output_pcm.data();
    const int converted = swr_convert(swr, &dest, bound,
        const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
    uint32_t bytes = 0;
    if (converted < 0 || !codec::aac_pcm_bytes(static_cast<uint32_t>(converted),
            output_channels, is_sbr, max_samples * output_channels * sizeof(int16_t), bytes)) {
        output_pcm.clear();
        has_frame = false;
        swr_free(&swr);
        return false;
    }
    output_pcm.resize(bytes);
    output_ready = true;
    return true;
}

bool AacDecoderState::receive(uint8_t *data, DecoderSize *size) {
    return receive_bounded(data, (is_sbr ? 2048 : 1024) * output_channels * sizeof(int16_t), size);
}

bool AacDecoderState::receive_bounded(uint8_t *data, uint32_t capacity, DecoderSize *size) {
    std::lock_guard<std::mutex> lock(codec_mutex);
    if (size)
        size->samples = 0;
    if (!prepare_output() || output_pcm.size() > capacity)
        return false;
    if (data && !output_pcm.empty())
        std::memcpy(data, output_pcm.data(), output_pcm.size());
    if (size)
        size->samples = static_cast<uint32_t>(output_pcm.size() / (output_channels * sizeof(int16_t)));
    return true;
}

void AacDecoderState::flush() {
    std::lock_guard<std::mutex> lock(codec_mutex);
    has_frame = false;
    output_ready = false;
    es_size_used = 0;
    output_pcm.clear();
    if (frame)
        av_frame_unref(frame);
    swr_free(&swr);
    if (context && avcodec_is_open(context))
        avcodec_flush_buffers(context);
}

uint32_t AacDecoderState::get_es_size() {
    return es_size_used;
}
