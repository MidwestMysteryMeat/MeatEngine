#pragma once
#include <cstdint>
#include <memory>

namespace meat {

// The slice's sound set. Samples are synthesized procedurally at init (no bundled
// audio files → no licensing), so this is a fixed enum rather than a registry.
enum class Sound : std::uint8_t {
    Gunshot,
    Footstep,
    Pickup,
    Hit,
    Explosion,
    UiClick,
    Count,
};

// Thin wrapper over miniaudio's high-level engine. Fire-and-forget playback of
// pre-generated sounds; mixing/device/threading are miniaudio's job. Client-side
// only — the server never makes sound. A failed init disables audio silently so
// a headless/audioless box still runs.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    void play(Sound sound, float volume = 1.0f); // 2D, non-positional (MVP)

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // pimpl: miniaudio stays out of the engine headers
};

} // namespace meat
