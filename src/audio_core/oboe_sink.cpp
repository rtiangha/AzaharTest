#include "oboe_sink.h"

#include <memory>
#include <mutex>
#include <oboe/Oboe.h>

#include "audio_core/audio_types.h"
#include "common/logging/log.h"

namespace AudioCore {

class OboeSink::Impl : public oboe::AudioStreamCallback {
public:
    Impl() = default;
    ~Impl() override {
        CloseStreamLocked();
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* oboeStream, void* audioData,
                                          int32_t numFrames) override {
        s16* outputBuffer = static_cast<s16*>(audioData);
        std::function<void(s16*, std::size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            callback = mCallback;
        }
        if (callback) {
            callback(outputBuffer, static_cast<std::size_t>(numFrames));
        }
        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* /* oboeStream */, oboe::Result error) override {
        if (error == oboe::Result::ErrorDisconnected) {
            LOG_INFO(Audio_Sink, "Restarting AudioStream after disconnect");
            std::lock_guard<std::mutex> lock(mMutex);
            StartLocked();
        } else {
            LOG_CRITICAL(Audio_Sink, "Error after close: {}", error);
        }
    }

    [[nodiscard]] bool start() {
        std::lock_guard<std::mutex> lock(mMutex);
        return StartLocked();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mMutex);
        CloseStreamLocked();
    }

    int32_t GetSampleRate() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mSampleRate;
    }

    void SetCallback(std::function<void(s16*, std::size_t)> cb) {
        std::lock_guard<std::mutex> lock(mMutex);
        mCallback = std::move(cb);
    }

private:
    // Caller must hold mMutex.
    [[nodiscard]] bool StartLocked() {
        CloseStreamLocked();

        auto result = OpenStreamLocked(oboe::SharingMode::Exclusive);
        if (result != oboe::Result::OK) {
            LOG_WARNING(Audio_Sink,
                        "Exclusive stream open failed ({}), retrying with shared mode",
                        oboe::convertToText(result));
            result = OpenStreamLocked(oboe::SharingMode::Shared);
        }
        if (result != oboe::Result::OK) {
            LOG_CRITICAL(Audio_Sink, "Error creating playback stream: {}",
                         oboe::convertToText(result));
            return false;
        }

        mSampleRate = mStream->getSampleRate();
        result = mStream->start();
        if (result != oboe::Result::OK) {
            LOG_CRITICAL(Audio_Sink, "Error starting playback stream: {}",
                         oboe::convertToText(result));
            CloseStreamLocked();
            return false;
        }
        return true;
    }

    // Caller must hold mMutex.
    oboe::Result OpenStreamLocked(oboe::SharingMode sharingMode) {
        oboe::AudioStreamBuilder builder;
        return builder.setSharingMode(sharingMode)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setAudioApi(oboe::AudioApi::Unspecified)
            ->setUsage(oboe::Usage::Game)
            ->setFormat(oboe::AudioFormat::I16)
            ->setFormatConversionAllowed(true)
            ->setSampleRate(mSampleRate)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::High)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setCallback(this)
            ->openStream(mStream);
    }

    // Caller must hold mMutex.
    void CloseStreamLocked() {
        if (mStream && mStream->getState() != oboe::StreamState::Closed) {
            auto stopResult = mStream->stop();
            auto closeResult = mStream->close();
            if (stopResult != oboe::Result::OK) {
                LOG_CRITICAL(Audio_Sink, "Error stopping playback stream: {}",
                             oboe::convertToText(stopResult));
            }
            if (closeResult != oboe::Result::OK) {
                LOG_CRITICAL(Audio_Sink, "Error closing playback stream: {}",
                             oboe::convertToText(closeResult));
            }
        }
        mStream.reset();
    }

    mutable std::mutex mMutex;
    std::shared_ptr<oboe::AudioStream> mStream;
    std::function<void(s16*, std::size_t)> mCallback;
    int32_t mSampleRate = native_sample_rate;
};

OboeSink::OboeSink(std::string_view /* device_id */) : impl(std::make_unique<Impl>()) {}
OboeSink::~OboeSink() {
    // Ensures resources are freed up when OboeSink is destroyed
    if (impl) {
        impl->stop();
    }
}

unsigned int OboeSink::GetNativeSampleRate() const {
    return impl->GetSampleRate();
}

void OboeSink::SetCallback(std::function<void(s16*, std::size_t)> cb) {
    impl->SetCallback(std::move(cb));
    if (!impl->start()) {
        LOG_CRITICAL(Audio_Sink, "Failed to start Oboe stream after SetCallback");
    }
}

std::vector<std::string> ListOboeSinkDevices() {
    return {"auto"};
}

} // namespace AudioCore
