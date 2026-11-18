#include "audio/AudioDevice.h"

#include <windows.h>
#include <xaudio2.h>

#include <array>
#include <atomic>
#include <format>
#include <vector>

#include "audio/Mixer.h"

namespace bh {

namespace {
constexpr int kFrames = 1024;
constexpr int kBuffers = 3;
}  // namespace

struct AudioDevice::Impl : public IXAudio2VoiceCallback {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    Mixer* mixer = nullptr;
    std::mutex* lock = nullptr;
    std::array<std::vector<float>, kBuffers> buffers;
    int next = 0;
    std::atomic<bool> stopping{false};
    bool comInit = false;

    void submit() {
        if (stopping) return;
        std::vector<float>& b = buffers[size_t(next)];
        next = (next + 1) % kBuffers;
        {
            std::lock_guard<std::mutex> g(*lock);
            mixer->render(b.data(), kFrames);
        }
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = UINT32(b.size() * sizeof(float));
        xb.pAudioData = reinterpret_cast<const BYTE*>(b.data());
        source->SubmitSourceBuffer(&xb);
    }

    // IXAudio2VoiceCallback
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { submit(); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

AudioDevice::AudioDevice() = default;
AudioDevice::~AudioDevice() { close(); }

bool AudioDevice::running() const { return impl_ && impl_->source; }

bool AudioDevice::open(Mixer& mixer, std::mutex& lock, std::string& error) {
    close();
    // (delay-loaded: make sure it is there before the first call into it)
    if (!LoadLibraryW(L"xaudio2_9.dll")) {
        error = "xaudio2_9.dll is not available";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->mixer = &mixer;
    impl->lock = &lock;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl->comInit = SUCCEEDED(co);
    HRESULT hr = XAudio2Create(&impl->xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        error = std::format("XAudio2Create failed (0x{:08x})", uint32_t(hr));
        if (impl->comInit) CoUninitialize();
        return false;
    }
    hr = impl->xa->CreateMasteringVoice(&impl->master, 2, Mixer::kRate);
    if (FAILED(hr)) {
        error = std::format("no audio device (CreateMasteringVoice 0x{:08x})", uint32_t(hr));
        impl->xa->Release();
        if (impl->comInit) CoUninitialize();
        return false;
    }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = Mixer::kRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 8;
    fmt.nAvgBytesPerSec = Mixer::kRate * 8;
    hr = impl->xa->CreateSourceVoice(&impl->source, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, impl.get());
    if (FAILED(hr)) {
        error = std::format("CreateSourceVoice failed (0x{:08x})", uint32_t(hr));
        impl->master->DestroyVoice();
        impl->xa->Release();
        if (impl->comInit) CoUninitialize();
        return false;
    }
    for (auto& b : impl->buffers) b.assign(size_t(kFrames) * 2, 0.0f);
    for (int i = 0; i < kBuffers; ++i) impl->submit();
    impl->source->Start(0);
    impl_ = std::move(impl);
    return true;
}

void AudioDevice::close() {
    if (!impl_) return;
    impl_->stopping = true;
    if (impl_->source) {
        impl_->source->Stop(0);
        impl_->source->DestroyVoice();
    }
    if (impl_->master) impl_->master->DestroyVoice();
    if (impl_->xa) impl_->xa->Release();
    if (impl_->comInit) CoUninitialize();
    impl_.reset();
}

}  // namespace bh
