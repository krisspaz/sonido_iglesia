#pragma once

#include "Biquad.h"
#include "DspParameters.h"
#include "TruePeakDetector.h"

#include <array>
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

    DspParameters& getParameters() noexcept { return parameters; }
    AdaptiveTargets& getAdaptiveTargets() noexcept { return adaptiveTargets; }
    DspMetrics& getMetrics() noexcept { return metrics; }

private:
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

    std::array<float, 4> bandEnvelope {};
    std::array<float, 4> bandGain { 1.0f, 1.0f, 1.0f, 1.0f };
    float limiterGain = 1.0f;
    float lastCompressorReduction = 0.0f;
    float lastLimiterReduction = 0.0f;
    double dryLoudnessSquare = 0.0;
    double wetLoudnessSquare = 0.0;
    float loudnessMatchCoefficient = 0.0f;
};
} // namespace churchstream
