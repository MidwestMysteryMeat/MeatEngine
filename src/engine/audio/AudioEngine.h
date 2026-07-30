#pragma once
#include <cstdint>
#include <memory>

#include <glm/vec3.hpp>

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
    void play(Sound sound, float volume = 1.0f); // 2D, non-positional (HUD / first-person)

    // Update the listener pose each frame from the local camera. Positional
    // playback (playAt) attenuates + pans relative to this. `forward`/`right` are
    // the camera's world axes (need not be normalized — only `right`'s direction
    // matters for panning). Cheap; safe to call every frame.
    void setListener(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& right);

    // Positional playback: gain falls off linearly with distance from the listener
    // (full within kMinDistance, silent at/beyond kMaxAudibleRadius — no voice is
    // even taken past the radius), and the voice is panned L/R by the source's
    // bearing relative to the listener. Use for world events (remote footsteps,
    // distant shots). Falls back to a centered 2D trigger if no listener is set.
    void playAt(Sound sound, const glm::vec3& worldPos, float volume = 1.0f);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // pimpl: miniaudio stays out of the engine headers
};

} // namespace meat
