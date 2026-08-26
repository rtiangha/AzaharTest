// Copyright 2025 Borked3DS Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <mutex>

#include "audio_core/oboe_input.h"
#include "audio_core/sink.h" // For auto_device_name
#include "common/logging/log.h"
#include "common/threadsafe_queue.h"

namespace AudioCore {

using SampleQueue = Common::SPSCQueue<Samples>;

struct OboeInput::Impl : public oboe::AudioStreamDataCallback,
                         public oboe::AudioStreamErrorCallback {
    oboe::AudioStream* stream = nullptr;
    SampleQueue sample_queue{};
    u8 sample_size_in_bytes = 0;
    InputParameters current_params{};
    std::mutex mutex;

    // Reused across callbacks to avoid allocating on the audio thread.
    std::vector<u8> scratch_buffer;

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* oboeStream, void* audioData,
                                          int32_t numFrames) override {
        if (!audioData || numFrames <= 0) {
            return oboe::DataCallbackResult::Continue;
        }

        // The stream is always opened as I16, so each frame is 2 bytes
        // regardless of the requested output sample_size_in_bytes.
        const auto* inputBuffer = static_cast<const int16_t*>(audioData);
        const std::size_t frameCount = static_cast<std::size_t>(numFrames);

        scratch_buffer.clear();

        if (sample_size_in_bytes == 1) {
            scratch_buffer.reserve(frameCount);
            for (std::size_t i = 0; i < frameCount; ++i) {
                scratch_buffer.push_back(
                    static_cast<u8>((static_cast<uint16_t>(inputBuffer[i]) >> 8) & 0xFF));
            }
        } else {
            // sample_size_in_bytes is validated to be 1 or 2 in StartSampling,
            // so this is always a safe, in-bounds copy of the I16 source data.
            const auto* data = reinterpret_cast<const u8*>(inputBuffer);
            scratch_buffer.assign(data, data + frameCount * sizeof(int16_t));
        }

        sample_queue.Push(scratch_buffer);
        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* /* oboeStream */, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOG_WARNING(Audio, "Oboe input stream disconnected.");
        }
    }
};

OboeInput::OboeInput(std::string device_id)
    : impl(std::make_unique<Impl>()), device_id(std::move(device_id)) {}

OboeInput::~OboeInput() {
    StopSampling();
}

void OboeInput::StartSampling(const InputParameters& params) {
    if (IsSampling()) {
        return;
    }

    if (params.sign == Signedness::Unsigned) {
        LOG_WARNING(
            Audio,
            "Application requested unsupported unsigned PCM format. Falling back to signed.");
    }

    const u8 requested_sample_size_in_bytes = static_cast<u8>(params.sample_size / 8);
    if (requested_sample_size_in_bytes != 1 && requested_sample_size_in_bytes != 2) {
        LOG_CRITICAL(Audio,
                     "Unsupported input sample size: {} bits. Only 8-bit and 16-bit are "
                     "supported by the Oboe input backend.",
                     params.sample_size);
        return;
    }

    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->current_params = params;
    impl->sample_size_in_bytes = requested_sample_size_in_bytes;

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setAudioApi(oboe::AudioApi::Unspecified)
        ->setFormat(oboe::AudioFormat::I16)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setSampleRate(params.sample_rate)
        ->setDataCallback(impl.get())
        ->setErrorCallback(impl.get());

    // Oboe doesn't support named device selection - use default device
    if (device_id != auto_device_name && !device_id.empty()) {
        LOG_WARNING(Audio, "Oboe input doesn't support specific device selection - using default");
    }

    oboe::Result result = builder.openStream(&impl->stream);
    if (result != oboe::Result::OK || !impl->stream) {
        LOG_CRITICAL(Audio, "Failed to open Oboe input stream: {}", static_cast<int>(result));
        StopSamplingLocked();
        return;
    }

    result = impl->stream->requestStart();
    if (result != oboe::Result::OK) {
        LOG_CRITICAL(Audio, "Failed to start Oboe input stream: {}", static_cast<int>(result));
        StopSamplingLocked();
        return;
    }
}

void OboeInput::StopSampling() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    StopSamplingLocked();
}

// Caller must hold impl->mutex.
void OboeInput::StopSamplingLocked() {
    if (impl->stream) {
        auto stopResult = impl->stream->stop();
        auto closeResult = impl->stream->close();
        if (stopResult != oboe::Result::OK) {
            LOG_CRITICAL(Audio, "Error stopping input stream: {}", static_cast<int>(stopResult));
        }
        if (closeResult != oboe::Result::OK) {
            LOG_CRITICAL(Audio, "Error closing input stream: {}", static_cast<int>(closeResult));
        }
        impl->stream = nullptr;
    }
}

bool OboeInput::IsSampling() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->stream && impl->stream->getState() == oboe::StreamState::Started;
}

void OboeInput::AdjustSampleRate(u32 sample_rate) {
    if (!IsSampling()) {
        return;
    }

    InputParameters new_params;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        new_params = impl->current_params;
    }
    new_params.sample_rate = sample_rate;
    StopSampling();
    StartSampling(new_params);
}

Samples OboeInput::Read() {
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

std::vector<std::string> ListOboeInputDevices() {
    // Oboe doesn't support enumerating input devices by name
    return {auto_device_name};
}

} // namespace AudioCore
