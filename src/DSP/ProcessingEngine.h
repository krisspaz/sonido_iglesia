#pragma once

#include "Biquad.h"
#include "DspParameters.h"
#include "TruePeakDetector.h"

#include <array>
#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace churchstream
{
class ProcessingEngine final
{
public:
    static constexpr int maximumChannels = 2;
    static constexpr int maximumLookaheadSamples = 512;

    void prepare(double newSampleRate, int maximumBlockSize, int channels);
    void reset() noexcept;
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    [[nodiscard]] int getLatencySamples() const noexcept { return lookaheadSamples; }
    [[nodiscard]] double getSampleRate() const noexcept { return sampleRate; }
    [[nodiscard]] bool isFailsafeActive() const noexcept { return failsafeBlend > 0.0f; }

    DspParameters& getParameters() noexcept { return parameters; }
    AdaptiveTargets& getAdaptiveTargets() noexcept { return adaptiveTargets; }
    DspMetrics& getMetrics() noexcept { return metrics; }

private:
    // Clears every recursive state that a non-finite sample can poison. The
    // dry delay line and the write position are deliberately preserved: they
    // carry the signal the failsafe path is currently playing, so wiping them
    // would punch a hole in the very audio meant to cover the fault.
    void resetProcessingState() noexcept;
    float processDcBlocker(int channel, float sample) noexcept;
    void updateLoudnessMatch(float dryMono, float wetMono) noexcept;
    void updateTargets(int numSamples) noexcept;
    void configureFilters() noexcept;
    static float clampControl(float value) noexcept;

    DspParameters parameters;
    AdaptiveTargets adaptiveTargets;
    DspMetrics metrics;

    double sampleRate = 48000.0;
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    int lookaheadSamples = 48;
    int delayWritePosition = 0;

    juce::AudioBuffer<float> dryBuffer;
    std::array<std::array<float, maximumLookaheadSamples>, maximumChannels> wetDelay {};
    std::array<std::array<float, maximumLookaheadSamples>, maximumChannels> dryDelay {};
    std::array<float, maximumChannels> dcPreviousInput {};
    std::array<float, maximumChannels> dcPreviousOutput {};
    TruePeakDetector truePeakDetector;

    Biquad rumbleFilter;
    Biquad warmthFilter;
    Biquad lowFilter;
    Biquad mudFilter;
    Biquad clarityFilter;
    Biquad harshFilter;
    Biquad sibilanceFilter;
    Biquad highFilter;
    // Low-passes the Side channel so it can be subtracted from itself. The
    // result is 1 - (1 - bassWidth) * LP(z): exactly unity above the corner and
    // exactly bassWidth at DC, with no crossover phase error between the two
    // paths because there is only one path.
    Biquad sideBassFilter;

    juce::dsp::LinkwitzRileyFilter<float> middleSplit;
    juce::dsp::LinkwitzRileyFilter<float> lowSplit;
    juce::dsp::LinkwitzRileyFilter<float> highSplit;
    juce::dsp::LinkwitzRileyFilter<float> lowGroupPhase;
    juce::dsp::LinkwitzRileyFilter<float> highGroupPhase;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> rumbleCutoff;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> warmthGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mudGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> clarityGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> harshGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sibilanceGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stereoWidth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> stereoBalance;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> dryMatchGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> coherenceWidth;

    std::array<float, 4> bandEnvelope {};
    std::array<float, 4> bandGain { 1.0f, 1.0f, 1.0f, 1.0f };
    float limiterGain = 1.0f;
    float lastCompressorReduction = 0.0f;
    float lastLimiterReduction = 0.0f;
    // The leveler measures programme RMS after EQ/compression but before the
    // true-peak limiter. It deliberately uses seconds-long time constants so
    // phrases stay natural and pauses do not turn into amplified room noise.
    float programmeLevelSquare = 0.0f;
    float programmeLevelGain = 1.0f;
    double dryLoudnessSquare = 0.0;
    double wetLoudnessSquare = 0.0;
    float loudnessMatchCoefficient = 0.0f;

    // Pearson correlation of the processed stereo pair, integrated in double
    // because the three running products differ by orders of magnitude on
    // near-mono programme. The mean is not removed: the DC blocker already
    // guarantees a zero-mean signal, which is what every correlation meter
    // assumes.
    double correlationLeftRight = 0.0;
    double correlationLeftSquare = 0.0;
    double correlationRightSquare = 0.0;
    float correlationCoefficient = 0.0f;
    float measuredCorrelation = 1.0f;

    // Watchdog. `failsafeBlend` is the crossfade position between the
    // processed output (0) and the delayed dry safety path (1). A fault arms
    // the hold; the processed path is only re-entered once the hold has run
    // out with no further faults, so a filter that keeps diverging cannot
    // oscillate in and out of bypass.
    float failsafeBlend = 0.0f;
    float failsafeIncrement = 1.0f;
    int failsafeHoldSamples = 0;
    int failsafeHoldLength = 0;
    bool failsafeStateCleared = true;
    uint32_t failsafeEngagementCount = 0;
    uint32_t nonFiniteInputCount = 0;
};
} // namespace churchstream
