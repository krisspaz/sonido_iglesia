#pragma once

#include "DSP/Biquad.h"

#include <array>
#include <atomic>
#include <cmath>

namespace churchstream
{
enum class StemRole : int
{
    voice = 0,
    music = 1,
    ambience = 2
};

// Lightweight per-stem chains that run before the three stems are summed into
// one mix. Each role gets a small, forgiving chain tuned for what that stem is
// for on a church stream:
//
//   voice:    HP 80 Hz (rumble from near mics), a de-esser on the sibilance
//             region, a gentle 3:1 compressor to catch surges, presence shelf.
//   music:    HP 40 Hz (subsonic from consoles and room noise), a compressor
//             that keeps the graves and the beat steady.
//   ambience: HP 120 Hz, a soft gate so pauses fall away, with the overall
//             level still fixed by the mixer's own gain.
//
// Everything is hardcoded, deterministic and real-time safe: coefficients are
// computed once in prepare() and the sample loop only does multiplies and
// filtering. Off by default so a live service is never touched until the
// operator turns it on and the console routing has been verified.
class StemProcessor final
{
public:
    static constexpr int roleCount = 3;

    void prepare(double newSampleRate) noexcept;
    void reset() noexcept;

    void setEnabled(bool shouldBeEnabled) noexcept;
    [[nodiscard]] bool isEnabled() const noexcept;

    // Processes one stereo sample of one stem in place. Identity when the
    // processor is disabled or the role is out of range.
    void processSample(StemRole role, float& channelLeft, float& channelRight) noexcept;

    // The gain (dB) the role's dynamics are currently applying. Zero when the
    // chain is effectively transparent, negative while it is compressing or
    // gating. Lets the UI show the per-stem action without touching audio.
    [[nodiscard]] float getAppliedGainDb(StemRole role) const noexcept;

private:
    struct Compressor
    {
        // On voice the threshold is low enough to catch sermon surges, on
        // music it holds the graves and the beat steady.
        float thresholdDb = -24.0f;
        float ratio = 3.0f;
        float attackSeconds = 0.012f;
        float releaseSeconds = 0.150f;
        float makeupDb = 0.0f;
        float detectorDb = -96.0f;
        float gainDb = 0.0f;
        float attackCoeff = 0.0f;
        float releaseCoeff = 0.0f;
    };

    struct Gate
    {
        float openDb = -42.0f;
        float closeDb = -50.0f;
        float attackSeconds = 0.004f;
        float releaseSeconds = 0.050f;
        float floorDb = -72.0f;
        float level = 0.0f;
        float gainDb = 0.0f;
        float attackCoeff = 0.0f;
        float releaseCoeff = 0.0f;
        float releaseLevelCoeff = 0.0f;
    };

    struct VoiceChain
    {
        Biquad highPass;
        Biquad deEsserBand;
        Biquad presence;
        float deEsserLevel = 0.0f;
        float deEsserReleaseCoeff = 0.0f;
        Compressor compressor;
    };

    struct MusicChain
    {
        Biquad highPass;
        Compressor compressor;
    };

    struct AmbienceChain
    {
        Biquad highPass;
        Gate gate;
    };

    void applyCompressor(Compressor& compressor, float& left, float& right) noexcept;
    void applyGate(Gate& gate, float& left, float& right) noexcept;
    static float dbOf(float magnitude) noexcept;
    static void setOnePole(double sampleRate, float seconds, float& coeff) noexcept;
    static void setExponentialRelease(double sampleRate, float seconds, float& coeff) noexcept;

    std::atomic<bool> enabled { false };
    std::array<std::atomic<float>, roleCount> appliedGainDb {};
    VoiceChain voice;
    MusicChain music;
    AmbienceChain ambience;
};
} // namespace churchstream