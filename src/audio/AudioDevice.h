#pragma once
// The window's sound output (part 3): XAudio2 plays the mixer. Three buffers of 1024 frames
// (~23 ms each) stay queued; when one finishes, XAudio2's thread mixes the next under the
// lock the game holds while it hands the mixer its events. If there is no audio device (or
// XAudio2 can't start) open() returns false and the game carries on silent.

#include <memory>
#include <mutex>
#include <string>

namespace bh {

class Mixer;

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    bool open(Mixer& mixer, std::mutex& lock, std::string& error);
    void close();
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bh
