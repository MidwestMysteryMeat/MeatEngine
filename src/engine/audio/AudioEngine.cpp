#include "engine/audio/AudioEngine.h"
#include "engine/core/Log.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267 4456 4457 4996) // miniaudio third-party, not /W4
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/glm.hpp> // length/dot/clamp for positional gain + pan

#include <array>
#include <cmath>
#include <vector>

namespace meat {
namespace {

constexpr int kSampleRate = 44100;
constexpr auto kSoundCount = static_cast<std::size_t>(Sound::Count);

// Positional falloff, in world units (voxels are 0.5 m, so 40 ≈ 20 m). Full
// volume within kMinDistance, then a linear ramp down to silence at the radius.
constexpr float kMaxAudibleRadius = 40.0f;
constexpr float kMinDistance = 1.5f;
// Never fully drop the far ear — a hard-panned mono voice sounds like an
// earbud fell out. 0.85 keeps a source directly to the side clearly biased
// without silencing the opposite channel.
constexpr float kMaxPan = 0.85f;

// Deterministic white noise so builds are reproducible (no rand()).
struct Noise {
    std::uint32_t s = 0x1234567u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 8388607.5f - 1.0f; // [-1,1)
    }
};

// Each sound is a short mono PCM buffer synthesized from noise + tones with an
// exponential envelope — punchy retro SFX with no sample assets.
std::vector<float> synth(Sound sound) {
    Noise n;
    const auto seconds = [](float s) { return static_cast<std::size_t>(s * kSampleRate); };
    std::vector<float> buf;

    switch (sound) {
    case Sound::Gunshot: { // noise burst, fast decay + a low body thump
        buf.resize(seconds(0.18f));
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            const float env = std::exp(-t * 34.0f);
            const float body = std::sin(t * 110.0f * 6.2831853f) * std::exp(-t * 22.0f);
            buf[i] = (n.next() * 0.8f + body * 0.5f) * env;
        }
        break;
    }
    case Sound::Explosion: { // longer, rumbly noise
        buf.resize(seconds(0.7f));
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            const float env = std::exp(-t * 5.5f);
            const float rumble = std::sin(t * 55.0f * 6.2831853f) * 0.6f;
            buf[i] = (n.next() * 0.7f + rumble) * env;
        }
        break;
    }
    case Sound::Footstep: { // short filtered noise tick
        buf.resize(seconds(0.06f));
        float lp = 0.0f;
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            lp += (n.next() - lp) * 0.25f; // one-pole low-pass = duller thud
            buf[i] = lp * std::exp(-t * 60.0f) * 0.6f;
        }
        break;
    }
    case Sound::Pickup: { // rising two-tone blip
        buf.resize(seconds(0.14f));
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            const float f = 660.0f + 440.0f * t / 0.14f;
            buf[i] = std::sin(t * f * 6.2831853f) * std::exp(-t * 10.0f) * 0.4f;
        }
        break;
    }
    case Sound::Hit: { // short high tick
        buf.resize(seconds(0.05f));
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            buf[i] = std::sin(t * 900.0f * 6.2831853f) * std::exp(-t * 45.0f) * 0.5f;
        }
        break;
    }
    case Sound::UiClick: {
        buf.resize(seconds(0.03f));
        for (std::size_t i = 0; i < buf.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            buf[i] = std::sin(t * 1400.0f * 6.2831853f) * std::exp(-t * 80.0f) * 0.35f;
        }
        break;
    }
    default:
        break;
    }
    return buf;
}

} // namespace

struct AudioEngine::Impl {
    ma_engine engine{};
    bool ready = false;
    // Each sound: its PCM, wrapped in an audio buffer that ma_sounds reference.
    std::array<std::vector<float>, kSoundCount> pcm;
    std::array<ma_audio_buffer, kSoundCount> buffers{};
    std::array<bool, kSoundCount> bufferOk{};
    // A small pool of voices per sound so rapid re-triggers overlap instead of
    // cutting each other off (auto-fire needs several gunshots at once).
    static constexpr int kVoices = 6;
    std::array<std::array<ma_sound, kVoices>, kSoundCount> voices{};
    std::array<int, kSoundCount> nextVoice{};

    // Listener pose, updated each frame from the camera. Default right = +X so
    // panning is sane before the first setListener call.
    glm::vec3 listenerPos{0.0f};
    glm::vec3 listenerFwd{0.0f, 0.0f, -1.0f};
    glm::vec3 listenerRight{1.0f, 0.0f, 0.0f};

    // Round-robin a voice from the pool and fire it at a given volume + pan.
    // Shared by 2D play() and positional playAt(); touches only the high-level
    // ma_sound handles (no allocation, no audio-callback-thread work).
    void trigger(std::size_t s, float volume, float pan) {
        ma_sound& v = voices[s][nextVoice[s]];
        nextVoice[s] = (nextVoice[s] + 1) % kVoices;
        ma_sound_stop(&v);
        ma_sound_seek_to_pcm_frame(&v, 0);
        ma_sound_set_pan(&v, pan);
        ma_sound_set_volume(&v, volume);
        ma_sound_start(&v);
    }

    ~Impl() {
        if (!ready) return;
        for (std::size_t s = 0; s < kSoundCount; ++s) {
            if (!bufferOk[s]) continue;
            for (auto& v : voices[s]) ma_sound_uninit(&v);
            ma_audio_buffer_uninit(&buffers[s]);
        }
        ma_engine_uninit(&engine);
    }
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::init() {
    if (ma_engine_init(nullptr, &m_impl->engine) != MA_SUCCESS) {
        log::warn("audio: engine init failed — running silent");
        return false;
    }
    for (std::size_t s = 0; s < kSoundCount; ++s) {
        m_impl->pcm[s] = synth(static_cast<Sound>(s));
        if (m_impl->pcm[s].empty()) continue;

        ma_audio_buffer_config bc = ma_audio_buffer_config_init(
            ma_format_f32, 1, m_impl->pcm[s].size(), m_impl->pcm[s].data(), nullptr);
        if (ma_audio_buffer_init(&bc, &m_impl->buffers[s]) != MA_SUCCESS) continue;
        m_impl->bufferOk[s] = true;

        for (auto& v : m_impl->voices[s]) {
            ma_sound_init_from_data_source(&m_impl->engine, &m_impl->buffers[s], 0, nullptr,
                                           &v);
        }
    }
    m_impl->ready = true;
    log::info("audio: engine up ({} synthesized sounds)", kSoundCount);
    return true;
}

void AudioEngine::setListener(const glm::vec3& pos, const glm::vec3& forward,
                              const glm::vec3& right) {
    m_impl->listenerPos = pos;
    m_impl->listenerFwd = forward;
    m_impl->listenerRight = right;
}

void AudioEngine::play(Sound sound, float volume) {
    if (!m_impl->ready) return;
    const auto s = static_cast<std::size_t>(sound);
    if (s >= kSoundCount || !m_impl->bufferOk[s]) return;
    m_impl->trigger(s, volume, 0.0f); // 2D: centered (reset pan — voices are shared with playAt)
}

void AudioEngine::playAt(Sound sound, const glm::vec3& worldPos, float volume) {
    if (!m_impl->ready) return;
    const auto s = static_cast<std::size_t>(sound);
    if (s >= kSoundCount || !m_impl->bufferOk[s]) return;

    const glm::vec3 delta = worldPos - m_impl->listenerPos;
    const float dist = glm::length(delta);
    if (dist >= kMaxAudibleRadius) return; // beyond earshot — don't even take a voice

    // Linear distance attenuation: 1.0 within kMinDistance, ramping to 0 at the radius.
    float gain = 1.0f;
    if (dist > kMinDistance)
        gain = 1.0f - (dist - kMinDistance) / (kMaxAudibleRadius - kMinDistance);
    gain = glm::clamp(gain, 0.0f, 1.0f);
    if (gain <= 0.0f) return;

    // Stereo pan: project the (normalized) source direction onto the listener's
    // right axis. +1 = to the right, -1 = to the left, ~0 = ahead/behind/overhead.
    float pan = 0.0f;
    if (dist > 1e-4f)
        pan = glm::clamp(glm::dot(delta / dist, m_impl->listenerRight), -1.0f, 1.0f) * kMaxPan;

    m_impl->trigger(s, volume * gain, pan);
}

} // namespace meat
