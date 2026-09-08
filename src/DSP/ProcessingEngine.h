#pragma once

#include "Biquad.h"
#include "DspParameters.h"
#include "DynamicBand.h"
#include "KWeighting.h"
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
    KWeighting levelerWeighting;
    float lastCompressorReduction = 0.0f;
    float lastLimiterReduction = 0.0f;
    // The leveler measures programme RMS after EQ/compression but before the
    // true-peak limiter. It deliberately uses seconds-long time constants so
    // phrases stay natural and pauses do not turn into amplified room noise.
    float programmeLevelSquare = 0.0f;
    float programmeLevelGain = 1.0f;

    // One-dimensional Kalman estimate of programme level in dB. The point is
    // not the maths but the adaptive gain: while the level is stable the
    // covariance collapses and the estimate barely moves, which is what stops a
    // compressor from breathing between phrases. When the innovation stays
    // large -- worship ending, a pastor starting to pray -- the process noise
    // is raised and the same filter turns into a fast one for as long as the
    // section change lasts.
    float programmeLevelEstimateDb = -100.0f;
    float programmeLevelCovariance = 400.0f;
    float programmeInnovationAverage = 0.0f;
    bool programmeLevelInitialised = false;
    // Gating with the shape of BS.1770 (absolute floor plus a relative gate
    // below the running programme level) applied to the leveler's RMS detector.
    // It is not a conformant loudness measurement -- there is no K-weighting
    // and no 400 ms blocks -- it is the same gating idea used to stop pauses
    // from being levelled up into air-conditioning noise.
    float programmeLoudnessAverage = -100.0f;
    // Counts gated samples so the running average starts as a true cumulative
    // mean and only then degrades into a 30 s window. A plain exponential
    // average needs minutes to become meaningful, and until it does the
    // relative gate sits far too low to exclude anything.
    float loudnessSampleCount = 0.0f;
    // A pause and a quieter section look identical to the relative gate: both
    // drop below it. What separates them is how long they last. Anything
    // shorter than the hold is treated as a pause and frozen through; anything
    // longer is accepted as the new programme, and the reference the gate is
    // relative to is rebased onto it so the leveler can track it.
    float gateClosedSamples = 0.0f;
    float gateHoldSamples = 0.0f;
    // Counts down after the gate accepts a new section, and is the only thing
    // that lets the estimate accelerate downwards. Speeding up on any fall
    // means a pause is chased for as long as the gate takes to classify it,
    // and that fall is exactly the room tone nobody wants amplified.
    float sectionRebaseSamples = 0.0f;
    float sectionRebaseLength = 0.0f;
    // Duration alone cannot separate a quieter section from room tone: four
    // seconds of air conditioning outlasts any sensible hold. What separates
    // them is that speech moves and a room does not. This tracks how much the
    // measured level varies around its own short-term trend, which is several
    // dB for anything anyone is saying and close to zero for a fan.
    // The level the gate decides on, integrated over roughly the 400 ms block
    // BS.1770 gates on. The Kalman still measures the fast 50 ms detector: a
    // gate driven by that detector opens and closes on every syllable, and
    // each closure freezes the estimate for a few milliseconds, which between
    // them halve how fast a section change can be tracked.
    float gateLevelDb = -100.0f;
    float gateLevelCoefficient = 0.0f;
    float programmeLevelTrend = -100.0f;
    float levelVariationDb = 0.0f;
    float levelTrendCoefficient = 0.0f;
    bool levelerGateOpen = false;
    // A closed gate freezes the leveler, which is right for a prayer pause but
    // wrong for the end of a service: holding +15 dB armed means the next thing
    // through the microphone gets amplified. Real silence, below the absolute
    // gate rather than merely below the relative one, therefore slides the
    // estimate back towards the target so the gain returns to unity.
    float silenceSamples = 0.0f;
    float silenceReleaseSamples = 0.0f;
    float silenceReleaseGain = 0.0f;
    float kalmanSteadyGain = 0.0f;
    float kalmanFastGain = 0.0f;
    float innovationCoefficient = 0.0f;
    float loudnessAverageCoefficient = 0.0f;
    float levelDetectorCoefficient = 0.0f;
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
