#pragma once

#include "Biquad.h"
#include "DspParameters.h"
#include "DynamicBand.h"
#include "KWeighting.h"
#include "MonoSpread.h"
#include "ProgrammeLevelTracker.h"
#include "ToneMatch.h"
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
    // One block more generous than the true-peak lookahead needs, because the
    // delay lines also have to cover the oversampler's own latency.
    static constexpr int maximumLookaheadSamples = 1024;

    void prepare(double newSampleRate, int maximumBlockSize, int channels);
    void reset() noexcept;
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    // The dry route and the reported latency both use `lineLength`. The wet
    // route is re-aligned to it: the oversampler already delays the wet lane by
    // its own latency before this stage, so the wet ring is one oversampler
    // latency shorter and both lanes leave the block one `lineLength` after
    // their input sample arrived.
    [[nodiscard]] int getLatencySamples() const noexcept { return lineLength; }
    [[nodiscard]] double getSampleRate() const noexcept { return sampleRate.load(std::memory_order_acquire); }
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
    void configureDynamicBands() noexcept;
    void updateLoudnessMatch(float dryMono, float wetMono) noexcept;
    void updateTargets(int numSamples) noexcept;
    void configureFilters() noexcept;
    static float clampControl(float value) noexcept;

    DspParameters parameters;
    AdaptiveTargets adaptiveTargets;
    DspMetrics metrics;

    std::atomic<double> sampleRate { 48000.0 };
    int preparedBlockSize = 0;
    int preparedChannels = 0;
    int lookaheadSamples = 48;
    // The delay lines and the limiter window span both the true-peak lookahead
    // and the oversampler latency, so `lookaheadSamples` is not what the host
    // is told. `lineLength` is.
    int lineLength = 48;
    int wetDelayLength = 48;
    int oversamplerLatencySamples = 0;
    int wetDelayWritePosition = 0;
    int dryDelayWritePosition = 0;

    juce::AudioBuffer<float> dryBuffer;
    juce::AudioBuffer<float> preSaturationBuffer;
    // Saturating at 4x the stream rate pushes the aliases from the tanh past
    // 4x Nyquist, where the oversampler's own decimation filter removes them
    // before anything folds back into the audible range. It runs even when the
    // saturation itself is disabled so the reported latency never changes while
    // the operator toggles it.
    juce::dsp::Oversampling<float> oversampling { maximumChannels, 2,
                                                  juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                                                  true, true };
    std::array<std::array<float, maximumLookaheadSamples>, maximumChannels> wetDelay {};
    std::array<std::array<float, maximumLookaheadSamples>, maximumChannels> dryDelay {};
    std::array<float, maximumChannels> dcPreviousInput {};
    std::array<float, maximumChannels> dcPreviousOutput {};
    TruePeakDetector truePeakDetector;
    // These detectors receive the already-split crossover groups below. They
    // intentionally do not own filters or an alternate split tree.
    static constexpr int multibandLimiterBandCount = 3;
    std::array<TruePeakDetector, multibandLimiterBandCount> multibandTruePeakDetectors;

    // Fourth-order Butterworth, as two cascaded sections. Second order at
    // 20 Hz was doing essentially nothing about what actually dirties a church
    // stream: handling noise, air conditioning and stage thump all sit between
    // 40 and 90 Hz, where 12 dB/octave is not enough to matter.
    std::array<Biquad, 2> rumbleFilters;
    ToneMatch toneMatch;
    Biquad warmthFilter;
    Biquad lowFilter;
    Biquad mudFilter;
    Biquad clarityFilter;
    Biquad harshFilter;
    Biquad sibilanceFilter;
    Biquad highFilter;

    // Dynamic EQ. The static sections above set the tone; these only act while
    // the problem is actually present, which is what stops a fixed 240 Hz cut
    // from thinning the kick for the whole service just because one speaker
    // was boomy.
    enum DynamicBandIndex : int { dynamicMud = 0, dynamicHarsh, dynamicSibilance, dynamicBandCount };
    std::array<DynamicBand, dynamicBandCount> dynamicBands;
    // Broadband reference the sustained bands are judged against. The de-esser
    // is judged against its own average instead, and does not use this.
    float broadbandReferenceSquare = 0.0f;
    float broadbandReferenceCoefficient = 0.0f;
    // High-passes the Side channel. Subtracting a low-passed copy instead was
    // the obvious approach and it does not work: 1 - LP(z) of a second-order
    // Butterworth is not a high-pass, because the low-passed copy is phase
    // shifted, and at half the corner frequency the two paths cancel to only
    // -4 dB instead of the -15 dB the magnitude response suggests. A real
    // high-pass in cascade gives 24 dB/octave with no such surprise.
    // Two Linkwitz-Riley fourth-order sections in a row (48 dB/oct): the low
    // Side has to be gone before a phone speaker cancels it, while the high
    // Side keeps its presence untouched.
    std::array<Biquad, 4> sideBassFilters;
    MonoSpread monoSpread;

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

    // Mean square, not amplitude: a compressor that follows peaks reacts to
    // transients rather than to how loud the band actually is, which is the
    // usual reason a multiband compressor breathes on speech.
    std::array<float, 4> bandEnvelopeSquare {};
    std::array<float, 4> bandGain { 1.0f, 1.0f, 1.0f, 1.0f };
    // Slow average of the reduction each band is applying, added back as
    // makeup. Without it every dB of compression is a dB less output, so loud
    // passages get quieter instead of denser and the whole engine sounds like
    // it is taking life away rather than holding the mix together. The detector
    // runs on the uncompressed band, so this cannot form a loop.
    std::array<float, 4> bandAverageGain { 1.0f, 1.0f, 1.0f, 1.0f };
    float makeupCoefficient = 0.0f;
    // BODY: a heavily compressed copy of the recombined programme, shaped
    // towards the low mids and added back underneath it. Quiet passages and
    // the sustain of every note come up, peaks barely move, which is what
    // fills in a worship mix that sounds empty at the same loudness.
    std::array<Biquad, 2> bodyFilters;
    float bodyEnvelopeSquare = 0.0f;
    float bodyGain = 1.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bodyMix;
    float limiterGain = 1.0f;
    // Sliding minimum of the limiter gain across the lookahead window, then
    // smoothed. Applying the instantaneous gain to the delayed sample was a
    // one-sample step in gain, and a step is broadband energy: with a 4x
    // oversampled detector firing constantly on dense programme that is
    // audible as grit on the low end. The minimum is what keeps the smoothing
    // safe, because it holds the reduction for the whole window the peak can
    // be in rather than letting the smoother chase a value that has moved on.
    std::array<float, maximumLookaheadSamples> limiterGainHistory {};
    int limiterHistoryPosition = 0;
    float limiterWindowMinimum = 1.0f;
    float limiterSmoothedGain = 1.0f;
    float limiterSmoothingCoefficient = 0.0f;
    std::array<float, multibandLimiterBandCount> multibandLimiterGain { 1.0f, 1.0f, 1.0f };
    std::array<float, multibandLimiterBandCount> lastMultibandLimiterReduction {};
    // Blocks DC without also removing the bottom octave. The old fixed 0.995
    // was a first-order high-pass at about 38 Hz at 48 kHz, which is real
    // tonal shaping hiding inside something named after a safety measure, and
    // it moved with the sample rate.
    float dcBlockerCoefficient = 0.9993f;
    float lastCompressorReduction = 0.0f;
    float lastLimiterReduction = 0.0f;
    // Broadcast leveller, split around the compressor the way a broadcast
    // processor is. The gated, K-weighted estimate is taken on the programme
    // entering the crossover, and the part of the leveller gain that brings it
    // to a fixed working level is applied there (`inputLevelGain`), so the
    // compressor always works on the same programme level whatever the console
    // sends. The rest -- working level to the operator's target, plus the
    // loudness the compression itself took away -- is applied after it
    // (`outputLevelGain`). Measuring after the compressor instead, as this used
    // to, left the compressor judging raw console level against fixed
    // thresholds: on a -35 dBFS feed it never engaged at all.
    ProgrammeLevelTracker levelTracker;
    float levelTotalDb = 0.0f;
    float levelInputDb = 0.0f;
    float inputLevelGain = 1.0f;
    float outputLevelGain = 1.0f;
    // Loudness the compressor removed, as a ratio of K-weighted power entering
    // the crossover to power leaving the wet path. A ratio of two averages over
    // the same span, so it follows what the compressor did and not how the
    // programme moved; integrated only while the gate is open.
    KWeighting compressionOutputWeighting;
    float compressionInputSquare = 0.0f;
    float compressionOutputSquare = 0.0f;
    float compressionAverageCoefficient = 0.0f;
    float compensationDb = 0.0f;
    float compensationSmoothing = 0.0f;
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
