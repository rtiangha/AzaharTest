// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <mutex>
#include <vector>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "audio_core/audio_types.h"
#include "audio_core/openal_sink.h"
#include "common/logging/log.h"

namespace AudioCore {

struct OpenALSink::Impl {
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    ALuint buffer = 0;
    ALuint source = 0;
    bool initialized = false;

    std::mutex mutex;
    std::function<void(s16*, std::size_t)> cb;

    static ALsizei Callback(void* impl_, void* buffer, ALsizei buffer_size_in_bytes);
};

OpenALSink::OpenALSink(std::string device_name) : impl(std::make_unique<Impl>()) {
    impl->device = alcOpenDevice(
        device_name != auto_device_name && !device_name.empty() ? device_name.c_str() : nullptr);
    if (!impl->device) {
        LOG_CRITICAL(Audio_Sink, "alcOpenDevice failed.");
        Close();
        return;
    }

    impl->context = alcCreateContext(impl->device, nullptr);
    if (impl->context == nullptr) {
        LOG_CRITICAL(Audio_Sink, "alcCreateContext failed: {}", alcGetError(impl->device));
        Close();
        return;
    }

    if (alcMakeContextCurrent(impl->context) == ALC_FALSE) {
        LOG_CRITICAL(Audio_Sink, "alcMakeContextCurrent failed: {}", alcGetError(impl->device));
        Close();
        return;
    }

    if (alIsExtensionPresent("AL_SOFT_callback_buffer") == AL_FALSE) {
        if (alGetError() != AL_NO_ERROR) {
            LOG_CRITICAL(Audio_Sink, "alIsExtensionPresent failed: {}", alGetError());
        } else {
            LOG_CRITICAL(Audio_Sink, "Missing required extension AL_SOFT_callback_buffer.");
        }
        Close();
        return;
    }

    alGenBuffers(1, &impl->buffer);
    if (alGetError() != AL_NO_ERROR) {
        LOG_CRITICAL(Audio_Sink, "alGetError failed: {}", alGetError());
        Close();
        return;
    }

    alGenSources(1, &impl->source);
    if (alGetError() != AL_NO_ERROR) {
        LOG_CRITICAL(Audio_Sink, "alGenSources failed: {}", alGetError());
        Close();
        return;
    }

    auto alBufferCallbackSOFT =
        reinterpret_cast<LPALBUFFERCALLBACKSOFT>(alGetProcAddress("alBufferCallbackSOFT"));
    if (!alBufferCallbackSOFT) {
        LOG_CRITICAL(Audio_Sink, "Failed to resolve alBufferCallbackSOFT proc address.");
        Close();
        return;
    }

    alBufferCallbackSOFT(impl->buffer, AL_FORMAT_STEREO16, native_sample_rate,
                         reinterpret_cast<ALBUFFERCALLBACKTYPESOFT>(&Impl::Callback), impl.get());

    if (alGetError() != AL_NO_ERROR) {
        LOG_CRITICAL(Audio_Sink, "alBufferCallbackSOFT failed: {}", alGetError());
        Close();
        return;
    }

    alSourcei(impl->source, AL_BUFFER, static_cast<ALint>(impl->buffer));
    if (alGetError() != AL_NO_ERROR) {
        LOG_CRITICAL(Audio_Sink, "alSourcei(AL_BUFFER) failed: {}", alGetError());
        Close();
        return;
    }

    if (alIsExtensionPresent("AL_SOFT_direct_channels") == AL_TRUE) {
        // Set up direct channels to bypass processing spatialization and other effects we don't
        // need.
        alSourcei(impl->source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
        if (alGetError() != AL_NO_ERROR) {
            LOG_CRITICAL(Audio_Sink, "alSourcei(AL_DIRECT_CHANNELS_SOFT) failed: {}", alGetError());
            Close();
            return;
        }
    } else {
        LOG_WARNING(Audio_Sink,
                    "AL_SOFT_direct_channels not present, audio latency may be higher.");
    }

    alSourcePlay(impl->source);
    if (alGetError() != AL_NO_ERROR) {
        LOG_CRITICAL(Audio_Sink, "alSourcePlay failed: {}", alGetError());
        Close();
        return;
    }

    impl->initialized = true;
}

OpenALSink::~OpenALSink() {
    Close();
}

void OpenALSink::Close() {
    if (impl->source) {
        alSourceStop(impl->source);
        alDeleteSources(1, &impl->source);
        impl->source = 0;
    }
    if (impl->buffer) {
        alDeleteBuffers(1, &impl->buffer);
        impl->buffer = 0;
    }
    if (impl->context) {
        alcDestroyContext(impl->context);
        impl->context = nullptr;
    }
    if (impl->device) {
        alcCloseDevice(impl->device);
        impl->device = nullptr;
    }
    impl->initialized = false;
}

unsigned int OpenALSink::GetNativeSampleRate() const {
    return native_sample_rate;
}

void OpenALSink::SetCallback(std::function<void(s16*, std::size_t)> cb) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->cb = std::move(cb);
}

bool OpenALSink::IsInitialized() const {
    return impl->initialized;
}

ALsizei OpenALSink::Impl::Callback(void* impl_, void* buffer, ALsizei buffer_size_in_bytes) {
    auto* impl = static_cast<Impl*>(impl_);
    if (!impl) {
        return 0;
    }

    std::function<void(s16*, std::size_t)> callback;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        callback = impl->cb;
    }
    if (!callback) {
        return 0;
    }

    // AL_FORMAT_STEREO16: 2 channels, 16-bit samples per channel.
    const std::size_t num_frames = buffer_size_in_bytes / (2 * sizeof(s16));
    callback(reinterpret_cast<s16*>(buffer), num_frames);

    return buffer_size_in_bytes;
}

std::vector<std::string> ListOpenALSinkDevices() {
    const char* devices_str;
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") != AL_FALSE) {
        devices_str = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
    } else if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") != AL_FALSE) {
        devices_str = alcGetString(nullptr, ALC_DEVICE_SPECIFIER);
    } else {
        LOG_WARNING(Audio_Sink,
                    "Missing OpenAL device enumeration extensions, cannot list audio devices.");
        return {};
    }

    if (!devices_str || *devices_str == '\0') {
        return {};
    }

    std::vector<std::string> device_list;
    while (*devices_str != '\0') {
        device_list.emplace_back(devices_str);
        devices_str += strlen(devices_str) + 1;
    }
    return device_list;
}

} // namespace AudioCore
