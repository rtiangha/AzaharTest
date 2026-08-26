// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <cubeb/cubeb.h>
#include "audio_core/cubeb_input.h"
#include "audio_core/input.h"
#include "audio_core/sink.h"
#include "common/logging/log.h"
#include "common/threadsafe_queue.h"

namespace AudioCore {

using SampleQueue = Common::SPSCQueue<Samples>;

struct CubebInput::Impl {
    // ctx is created lazily on first StartSampling and kept alive for the
    // lifetime of the object, so repeated start/stop cycles (e.g. from
    // AdjustSampleRate) don't pay the cost of re-initializing the cubeb
    // backend each time. Only ~CubebInput destroys it.
    cubeb* ctx = nullptr;
    cubeb_stream* stream = nullptr;

    SampleQueue sample_queue{};
    u8 sample_size_in_bytes = 0;
    std::mutex mutex;

    static long DataCallback(cubeb_stream* stream, void* user_data, const void* input_buffer,
                             void* output_buffer, long num_frames);
    static void StateCallback(cubeb_stream* stream, void* user_data, cubeb_state state);
};

CubebInput::CubebInput(std::string device_id)
    : impl(std::make_unique<Impl>()), device_id(std::move(device_id)) {}

CubebInput::~CubebInput() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    StopStreamLocked();
    if (impl->ctx) {
        cubeb_destroy(impl->ctx);
        impl->ctx = nullptr;
    }
}

void CubebInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }

    // Cubeb apparently only supports signed 16 bit PCM (and float32 which the 3ds doesn't support)
    // TODO: Resample the input stream.
    if (params.sign == Signedness::Unsigned) {
        LOG_WARNING(
            Audio,
            "Application requested unsupported unsigned pcm format. Falling back to signed.");
    }

    const u8 requested_sample_size_in_bytes = static_cast<u8>(params.sample_size / 8);
    if (requested_sample_size_in_bytes != 1 && requested_sample_size_in_bytes != 2) {
        LOG_CRITICAL(Audio,
                     "Unsupported input sample size: {} bits. Only 8-bit and 16-bit are "
                     "supported by the cubeb input backend.",
                     params.sample_size);
        return;
    }

    std::lock_guard<std::mutex> lock(impl->mutex);
    parameters = params;
    impl->sample_size_in_bytes = requested_sample_size_in_bytes;

    if (!impl->ctx) {
        auto init_result = cubeb_init(&impl->ctx, "Azahar Input", nullptr);
        if (init_result != CUBEB_OK) {
            LOG_CRITICAL(Audio, "cubeb_init failed: {}", init_result);
            return;
        }
    }

    cubeb_devid input_device = nullptr;
    if (device_id != auto_device_name && !device_id.empty()) {
        cubeb_device_collection collection;
        if (cubeb_enumerate_devices(impl->ctx, CUBEB_DEVICE_TYPE_INPUT, &collection) == CUBEB_OK) {
            const auto collection_end = collection.device + collection.count;
            const auto device = std::find_if(
                collection.device, collection_end, [this](const cubeb_device_info& info) {
                    return info.friendly_name != nullptr && device_id == info.friendly_name;
                });
            if (device != collection_end) {
                input_device = device->devid;
            }
            cubeb_device_collection_destroy(impl->ctx, &collection);
        } else {
            LOG_WARNING(Audio_Sink,
                        "Audio input device enumeration not supported, using default device.");
        }
    }

    cubeb_stream_params input_params = {
        .format = CUBEB_SAMPLE_S16LE,
        .rate = params.sample_rate,
        .channels = 1,
        .layout = CUBEB_LAYOUT_UNDEFINED,
    };

    u32 latency_frames = 512; // Firefox default
    auto latency_result = cubeb_get_min_latency(impl->ctx, &input_params, &latency_frames);
    if (latency_result != CUBEB_OK) {
        LOG_WARNING(
            Audio, "cubeb_get_min_latency failed, falling back to default latency of {} frames: {}",
            latency_frames, latency_result);
    }

    auto stream_init_result = cubeb_stream_init(
        impl->ctx, &impl->stream, "Azahar Microphone", input_device, &input_params, nullptr,
        nullptr, latency_frames, Impl::DataCallback, Impl::StateCallback, impl.get());
    if (stream_init_result != CUBEB_OK) {
        LOG_CRITICAL(Audio, "cubeb_stream_init failed: {}", stream_init_result);
        StopStreamLocked();
        return;
    }

    auto start_result = cubeb_stream_start(impl->stream);
    if (start_result != CUBEB_OK) {
        LOG_CRITICAL(Audio, "cubeb_stream_start failed: {}", start_result);
        StopStreamLocked();
        return;
    }
}

void CubebInput::StopSampling() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    StopStreamLocked();
}

// Caller must hold impl->mutex. Leaves impl->ctx alive.
void CubebInput::StopStreamLocked() {
    if (impl->stream) {
        auto stop_result = cubeb_stream_stop(impl->stream);
        if (stop_result != CUBEB_OK) {
            LOG_ERROR(Audio, "Error stopping cubeb input stream: {}", stop_result);
        }
        cubeb_stream_destroy(impl->stream);
        impl->stream = nullptr;
    }
}

bool CubebInput::IsSampling() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->stream != nullptr;
}

void CubebInput::AdjustSampleRate(u32 sample_rate) {
    if (!IsSampling()) {
        return;
    }

    auto new_parameters = parameters;
    new_parameters.sample_rate = sample_rate;
    StopSampling();
    StartSampling(new_parameters);
}

Samples CubebInput::Read() {
    if (!IsSampling()) {
        return {};
    }

    Samples samples{};
    Samples queue;
    while (impl->sample_queue.Pop(queue)) {
        samples.insert(samples.end(), queue.begin(), queue.end());
    }
    return samples;
}

long CubebInput::Impl::DataCallback(cubeb_stream* /* stream */, void* user_data,
                                    const void* input_buffer, void* /* output_buffer */,
                                    long num_frames) {
    auto impl = static_cast<Impl*>(user_data);
    if (!impl) {
        return 0;
    }

    u8 sample_size_in_bytes;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        sample_size_in_bytes = impl->sample_size_in_bytes;
    }

    constexpr auto resample_s16_s8 = [](s16 sample) {
        return static_cast<u8>(static_cast<u16>(sample) >> 8);
    };

    const std::size_t frame_count = static_cast<std::size_t>(num_frames);
    std::vector<u8> samples{};
    samples.reserve(frame_count * sample_size_in_bytes);
    if (sample_size_in_bytes == 1) {
        // If the sample format is 8bit, then resample back to 8bit before passing back to core
        for (std::size_t i = 0; i < frame_count; i++) {
            s16 data;
            std::memcpy(&data, static_cast<const u8*>(input_buffer) + i * 2, 2);
            samples.push_back(resample_s16_s8(data));
        }
    } else {
        // Otherwise copy all of the samples to the buffer (which will be treated as s16 by core).
        // sample_size_in_bytes is validated to be 1 or 2 in StartSampling, and the stream is
        // always opened as S16LE (2 bytes/frame), so this is always an in-bounds copy.
        const u8* data = reinterpret_cast<const u8*>(input_buffer);
        samples.insert(samples.end(), data, data + frame_count * sizeof(int16_t));
    }
    impl->sample_queue.Push(samples);

    // returning less than num_frames here signals cubeb to stop sampling
    return num_frames;
}

void CubebInput::Impl::StateCallback(cubeb_stream* /* stream */, void* /* user_data */,
                                     cubeb_state /* state */) {}

std::vector<std::string> ListCubebInputDevices() {
    std::vector<std::string> device_list;
    cubeb* ctx;

    if (cubeb_init(&ctx, "Azahar Input Device Enumerator", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio, "cubeb_init failed");
        return {};
    }

    cubeb_device_collection collection;
    if (cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_INPUT, &collection) == CUBEB_OK) {
        for (std::size_t i = 0; i < collection.count; i++) {
            const cubeb_device_info& device = collection.device[i];
            if (device.state == CUBEB_DEVICE_STATE_ENABLED && device.friendly_name) {
                device_list.emplace_back(device.friendly_name);
            }
        }
        cubeb_device_collection_destroy(ctx, &collection);
    } else {
        LOG_WARNING(Audio_Sink, "Audio input device enumeration not supported.");
    }

    cubeb_destroy(ctx);
    return device_list;
}

} // namespace AudioCore
