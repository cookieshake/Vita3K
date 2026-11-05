#include <codec/state.h>
#include <codec/aac_output.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

static void check(bool good, const char *name) {
    if (!good) throw std::runtime_error(name);
}
static void frame(AacDecoderState &aac, int rate, int channels, int samples, AVSampleFormat format) {
    av_frame_unref(aac.frame);
    aac.frame->sample_rate = rate;
    av_channel_layout_default(&aac.frame->ch_layout, channels);
    aac.frame->format = format;
    aac.frame->nb_samples = samples;
    check(av_frame_get_buffer(aac.frame, 0) == 0, "allocate synthetic frame");
    if (format == AV_SAMPLE_FMT_FLTP) {
        for (int ch = 0; ch < channels; ++ch)
            std::fill_n(reinterpret_cast<float *>(aac.frame->extended_data[ch]), samples, 0.25f);
    } else {
        std::fill_n(reinterpret_cast<int16_t *>(aac.frame->data[0]), samples * channels, int16_t(8192));
    }
    aac.has_frame = true;
    aac.output_ready = false;
    aac.output_pcm.clear();
}
static void compressed_sbr_fixture(const char *path) {
    AVFormatContext *format = nullptr;
    check(avformat_open_input(&format, path, nullptr, nullptr) == 0, "open compressed SBR fixture");
    try {
        check(avformat_find_stream_info(format, nullptr) >= 0, "read compressed SBR stream info");
        const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        check(stream_index >= 0, "find compressed SBR audio stream");
        AVStream *stream = format->streams[stream_index];
        AVCodecParameters *parameters = stream->codecpar;
        check(parameters->codec_id == AV_CODEC_ID_AAC, "fixture audio stream is AAC");
        const uint32_t channels = parameters->ch_layout.nb_channels;
        check(channels > 0 && channels <= 2, "compressed SBR channel count");

        AacDecoderState aac(parameters->sample_rate ? parameters->sample_rate : 24000, channels, true);
        avcodec_free_context(&aac.context);
        aac.context = avcodec_alloc_context3(aac.codec);
        check(aac.context && avcodec_parameters_to_context(aac.context, parameters) == 0, "copy AAC stream parameters");
        check(avcodec_open2(aac.context, aac.codec, nullptr) == 0, "open AAC decoder for compressed fixture");

        AVPacket *packet = av_packet_alloc();
        check(packet != nullptr, "allocate AAC packet");
        bool decoded = false;
        uint32_t output_samples = 0;
        for (int packets = 0; packets < 40 && av_read_frame(format, packet) >= 0; ++packets) {
            if (packet->stream_index == stream_index && aac.send(packet->data, packet->size)) {
                std::array<uint8_t, 8192> pcm{};
                DecoderSize actual{};
                check(aac.receive_bounded(pcm.data(), static_cast<uint32_t>(pcm.size()), &actual),
                    "convert compressed SBR frame through production output path");
                check(actual.samples > 0 && actual.samples <= 2048, "compressed SBR output sample bound");
                check(aac.frame->sample_rate > 0 && aac.get(DecoderQuery::SAMPLE_RATE) == static_cast<uint32_t>(aac.frame->sample_rate),
                    "compressed SBR uses decoded frame rate");
                check(aac.frame->nb_samples > 1024 || aac.frame->sample_rate > parameters->sample_rate,
                    "compressed fixture exposes SBR-expanded frame output");
                std::printf("compressed HE-AAC packet: stream_rate=%d decoded_rate=%d decoded_samples=%d output_samples=%u channels=%u\n",
                    parameters->sample_rate, aac.frame->sample_rate, aac.frame->nb_samples, actual.samples, channels);
                output_samples += actual.samples;
                decoded = true;
            }
            av_packet_unref(packet);
            if (decoded)
                break;
        }
        av_packet_free(&packet);
        check(decoded && output_samples > 0, "decode at least one public compressed SBR packet");
    } catch (...) {
        avformat_close_input(&format);
        throw;
    }
    avformat_close_input(&format);
}

int main(int argc, char **argv) {
    try {
        for (uint32_t n : {0u, 2u, 8u, UINT32_MAX})
            check(!codec::valid_aac_frame_count(n), "AAC rejects unsupported N-frame request");
        check(codec::valid_aac_frame_count(1), "AAC accepts one frame");
        uint32_t bytes = 123;
        check(!codec::aac_pcm_bytes(UINT32_MAX, 2, true, UINT32_MAX, bytes) && bytes == 0, "sample overflow");
        check(!codec::aac_pcm_bytes(2048, 0, true, 4096, bytes), "invalid channels");

        AacDecoderState aac(24000, 1, true);
        frame(aac, 48000, 2, 2048, AV_SAMPLE_FMT_FLTP);
        // Simulate the real decoder changing its context after opening mono.
        av_channel_layout_uninit(&aac.context->ch_layout);
        av_channel_layout_default(&aac.context->ch_layout, 2);
        aac.context->sample_rate = 48000;
        std::array<uint8_t, 8192> pcm;
        pcm.fill(0xa5);
        DecoderSize query{}, actual{};
        check(aac.receive(nullptr, &query) && query.samples == 2048, "exact prepared size query");
        check(aac.receive_bounded(pcm.data(), 4096, &actual), "SBR bounded receive");
        check(actual.samples == 2048 && aac.get(DecoderQuery::CHANNELS) == 1, "preserve caller channels");
        uint32_t hle_reported_bytes = 0;
        check(codec::aac_pcm_bytes(actual.samples, aac.get(DecoderQuery::CHANNELS), true, 4096, hle_reported_bytes)
                && hle_reported_bytes == 4096,
            "HLE AAC output boundary reports the bounded mono byte count");
        check(aac.get(DecoderQuery::SAMPLE_RATE) == 48000, "use expanded SBR frame rate");
        check(std::any_of(pcm.begin(), pcm.begin() + 4096, [](auto x) { return x != 0xa5; }), "PCM written");
        check(std::all_of(pcm.begin() + 4096, pcm.end(), [](auto x) { return x == 0xa5; }), "tail canary intact");
        const auto first = pcm;
        pcm.fill(0xa5);
        check(aac.receive_bounded(pcm.data(), 4096, &actual) && pcm == first, "query/copy uses same conversion");
        pcm.fill(0xa5);
        actual.samples = 999;
        check(!aac.receive_bounded(pcm.data(), 4095, &actual) && actual.samples == 0, "one-byte shortage rejected");
        check(std::all_of(pcm.begin(), pcm.end(), [](auto x) { return x == 0xa5; }), "shortage writes nothing");
        check(!aac.receive_bounded(pcm.data(), 0, &actual), "zero capacity rejected");

        frame(aac, 44100, 2, 2048, AV_SAMPLE_FMT_S16);
        check(aac.receive_bounded(pcm.data(), 4096, &actual) && actual.samples == 2048, "format/rate transition");
        check(aac.resampler_format->format == AV_SAMPLE_FMT_S16 && aac.get(DecoderQuery::SAMPLE_RATE) == 44100, "resampler follows frame");
        aac.flush();
        check(!aac.receive_bounded(pcm.data(), 4096, &actual) && actual.samples == 0 && aac.get_es_size() == 0, "flush invalidates PCM");
        frame(aac, 48000, 2, 2048, AV_SAMPLE_FMT_FLTP);
        check(aac.receive_bounded(pcm.data(), 4096, &actual), "reinitialize after flush");
        const uint8_t invalid[] = {0xff, 0xff, 0xff, 0xff};
        check(!aac.send(invalid, sizeof(invalid)), "invalid compressed packet rejected");
        check(!aac.receive_bounded(pcm.data(), 4096, &actual) && actual.samples == 0 && aac.get_es_size() == 0, "failed send invalidates prior output");
        frame(aac, 48000, 2, 2049, AV_SAMPLE_FMT_FLTP);
        check(!aac.receive_bounded(pcm.data(), 4096, &actual), "SBR frame limit");

        AacDecoderState lc(24000, 1);
        frame(lc, 48000, 1, 1024, AV_SAMPLE_FMT_FLTP);
        check(lc.receive_bounded(pcm.data(), 2048, &actual), "LC actual resampling");
        check(actual.samples > 0 && actual.samples < 1024, "report actual converted count instead of nb_samples");
        check(lc.output_pcm.size() == actual.samples * 2, "LC bytes match conversion");
        AacDecoderState stereo(48000, 2, true);
        frame(stereo, 48000, 2, 2048, AV_SAMPLE_FMT_FLTP);
        check(stereo.receive_bounded(pcm.data(), 8192, &actual) && stereo.output_pcm.size() == 8192, "stereo SBR capacity");
        if (argc > 1) {
            compressed_sbr_fixture(argv[1]);
            std::puts("AAC compressed SBR fixture PASS: Fraunhofer HE-AAC packet decoded and bounded through production AAC output");
        }
        std::puts("AAC fixture PASS: synthetic AVFrame bounds, mono canary, staging/query, capacity, LC count, format/rate, flush/error, stereo");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
