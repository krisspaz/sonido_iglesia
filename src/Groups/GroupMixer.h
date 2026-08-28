#pragma once

#include "DSP/Biquad.h"
#include "SmartMaskingController.h"

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>

namespace churchstream
{
// Sums the three X32 stems into the streaming mix and, when explicitly
// enabled, applies Smart Masking to the music stem only.
//
// Masking is off by default. With separate groups the engine can lower just
// the bands where the music is covering the preaching instead of pulling the
// whole mix down, but wiring it to a live service before the console routing
// has been verified could mute or double a stem on air.
class GroupMixer final
{
public:
    void prepare(double newSampleRate) noexcept;
    void reset() noexcept;

    void setMaskingEnabled(bool shouldBeEnabled) noexcept
    {
        maskingEnabled.store(shouldBeEnabled, std::memory_order_release);
    }
    [[nodiscard]] bool isMaskingEnabled() const noexcept
    {
        return maskingEnabled.load(std::memory_order_acquire);
    }

    // Returns false and touches nothing when the routes are not usable, so the
    // caller can fall back to the plain stereo path.
    bool process(const float* const* inputs, int inputCount,
                 float* const* outputs, int outputCount, int sampleCount,
                 const GroupRoutingConfig& routes) noexcept;

    [[nodiscard]] MaskingDecision getDecision() const noexcept;

private:
    struct BandAnalyser
    {
        std::array<Biquad, 5> filters;
        std::array<double, 5> energy {};
        double totalSquares = 0.0;

        void configure(double sampleRate) noexcept;
        void reset() noexcept;
        void push(float sample) noexcept;
        [[nodiscard]] GroupFeatures finish(int sampleCount) noexcept;
    };

    double sampleRate = 48000.0;
    std::atomic<bool> maskingEnabled { false };
    BandAnalyser voiceAnalyser;
    BandAnalyser musicAnalyser;
    SmartMaskingController masking;
    std::array<Biquad, 2> musicMaskFilters;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> presenceGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> upperGain;

    mutable juce::SpinLock decisionLock;
    MaskingDecision decision;
};
} // namespace churchstream
