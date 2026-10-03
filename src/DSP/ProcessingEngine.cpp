#include "ProcessingEngine.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
// A 0.1 dB control margin keeps the rendered signal below the public -1 dBTP
// ceiling despite release interpolation and floating-point filter tolerance.
constexpr float limiterCeiling = 0.88104887f;
constexpr float multibandMinimumCeiling = limiterCeiling;

// Watchdog thresholds. Real programme stays far below both: the limiter holds
// -1 dBTP and the saturator is bounded, so +18 dBFS on the processed path can
// only mean a diverging filter. The input clamp keeps +12 dBFS of headroom
// over anything a console can legitimately send, and exists so an absurd
// sample cannot be handed straight to the dry safety path either.
constexpr float failsafeMagnitude = 8.0f;
constexpr float inputMagnitudeClamp = 4.0f;
constexpr double failsafeCrossfadeSeconds = 0.010;
// Long enough that a filter which keeps diverging cannot chatter in and out of
// the safety path, short enough that a one-off glitch does not cost a phrase.
constexpr double failsafeHoldSeconds = 0.500;

// Stereo coherence. Below `coherenceOnsetCorrelation` the pair starts losing
// energy when a phone sums it to mono, so the Side channel is pulled in
// proportionally rather than at a single threshold: a hard switch would be
// audible as the image snapping shut mid-song.
constexpr double correlationIntegrationSeconds = 0.400;
constexpr float coherenceOnsetCorrelation = 0.30f;
constexpr float coherenceWorstCorrelation = -0.20f;
constexpr float coherenceMinimumWidth = 0.55f;

// Leveler. The detector is K-weighted and the level it reports is an
// approximate LUFS, so the target is simply the operator's loudness setting
// instead of a second, unrelated constant. The two used to disagree: the
// leveler aimed at -19 dBFS RMS while the Smart Engine aimed the output gain at
// -14 LUFS, and because the leveler measures after that gain it cancelled it.
// Both were on by default, so the stream sat several dB under the operator's
// target with the two loops slowly working against each other.
//
// The recovery range is the total the leveller may apply, wherever in the chain
// that gain sits. The sermons of two recorded services sat at -32.7 and -38 LUFS
// against -17 and -28 for worship; +24 dB brings a -38 LUFS sermon to a -14
// target. The gate is what keeps that range off room tone: pauses freeze the
// gain and real silence returns it to unity.
constexpr float levelerMaximumBoostDb = 24.0f;
constexpr float levelerMaximumCutDb = -10.0f;
// Programme level the compressor works at. The leveller brings the programme
// here before the crossover, so the compressor thresholds below are relative to
// this rather than to whatever the console happens to send. Chosen so the
// programme peaks sit several dB under full scale even on 20 dB-crest speech,
// which keeps the saturation stage gentle.
constexpr float levelerWorkingLevelDb = -20.0f;
// How the loudness the compressor removes is given back after it. Slow, because
// it only has to follow the material (speech loses more than music), and
// bounded, because it is a correction and not a second leveller.
constexpr double compressionCompensationSeconds = 3.0;
constexpr double compressionCompensationSmoothingSeconds = 1.0;
constexpr float compressionCompensationRangeDb = 4.0f;
// Compressor. A hard knee with the programme sitting near the threshold is the
// classic source of breathing on speech, because every syllable crosses it.
constexpr float compressorKneeDb = 8.0f;
// Thresholds are set against the leveller's working level, not full scale. The
// old -5 to -20 dBFS range was absolute, and recorded services reach the engine
// at -30 to -45 dBFS RMS, so across two complete services the compressor
// averaged 0.002 dB of gain reduction: present, enabled and doing nothing. With
// the leveller in front, the programme arrives at the working level and these
// become a real operating point. DYNAMICS moves the threshold down and the ratio
// up together.
constexpr float compressorThresholdAtMinimumDb = levelerWorkingLevelDb - 2.0f;
constexpr float compressorThresholdRangeDb = 10.0f;
constexpr float compressorRatioAtMinimum = 1.4f;
constexpr float compressorRatioRange = 2.4f;
constexpr double compressorMakeupSeconds = 0.50;
// Not the full reduction. Returning all of it would undo the compression on
// anything sustained; returning most of it keeps the level while still letting
// the fast movement be controlled.
constexpr float compressorMakeupAmount = 0.80f;

// BODY, the parallel bus. Threshold sits well under the working level so the
// bus is compressing nearly all the time: at full BODY a passage 12 dB under
// the working level comes up by about 3.5 dB overall and 5.5 dB around 220 Hz,
// while a loud chorus gains about 1 dB, which the limiter and the leveller
// absorb. The shaping keeps what is added in the 150-500 Hz region a thin mix
// is missing, and out of the presence range where it would only add harshness.
// Below the floor the bus expands away instead, so it cannot lift room tone in
// a pause the way a plain parallel compressor does.
constexpr float bodyThresholdDb = levelerWorkingLevelDb - 12.0f;
constexpr float bodyFloorDb = bodyThresholdDb - 18.0f;
constexpr float bodyExpansionRatio = 2.0f;
constexpr float bodyRatio = 6.0f;
constexpr float bodyMaximumReductionDb = 40.0f;
constexpr float bodyMaximumMix = 0.50f;
constexpr float bodyCentreHz = 220.0f;
constexpr float bodyCentreQ = 0.70f;
constexpr float bodyCentreGainDb = 5.0f;
constexpr float bodyLowPassHz = 4500.0f;
constexpr double bodyAttackSeconds = 0.005;
constexpr double bodyReleaseSeconds = 0.180;

// Dynamic EQ. The two sustained bands are judged against the broadband
// programme, the de-esser against its own running average. Offsets are in dB
// relative to that reference and are starting points, not measurements: they
// were chosen so that ordinary programme sits just under the threshold and a
// real buildup crosses it. Reductions are deliberately small, because the
// static adaptive EQ is still doing the bulk of the tonal work.
constexpr float dynamicMudCentreHz = 240.0f;
constexpr float dynamicMudQ = 1.00f;
constexpr float dynamicMudOffsetDb = -7.0f;
constexpr float dynamicMudRatio = 2.5f;
constexpr float dynamicMudMaximumDb = 3.0f;

constexpr float dynamicHarshCentreHz = 3800.0f;
constexpr float dynamicHarshQ = 1.00f;
constexpr float dynamicHarshOffsetDb = -12.0f;
constexpr float dynamicHarshRatio = 2.5f;
constexpr float dynamicHarshMaximumDb = 3.0f;

// A sibilant lasts between 60 and 150 ms. The adaptive EQ moves at 2 to 10 Hz
// with a 0.75 s ramp, so what it was doing at 7.4 kHz could not have been
// de-essing: it either missed the sibilant entirely or, once engaged, sat on
// the whole top end for seconds afterwards.
constexpr float deEsserCentreHz = 7000.0f;
constexpr float deEsserQ = 2.00f;
constexpr float deEsserOffsetDb = 5.0f;
constexpr float deEsserRatio = 4.0f;
constexpr float deEsserMaximumDb = 6.0f;
constexpr double deEsserAttackSeconds = 0.0008;
constexpr double deEsserReleaseSeconds = 0.050;
constexpr double dynamicSustainedAttackSeconds = 0.020;
constexpr double dynamicSustainedReleaseSeconds = 0.150;
constexpr double broadbandReferenceSeconds = 0.200;

// The crossover corner above which the compressor still has a band. This is
// NOT where saturation stops anymore: saturation runs on the full recombined
// wet path, oversampled 4x. At 48 kHz a plain tanh folds the harmonics of
// anything above about 8 kHz back into the audible range as inharmonic
// aliasing, which is exactly the kind of fault that makes a stream sound grainy
// on cymbals and sibilants without being identifiable; the oversampler pushes
// those harmonics past 4x Nyquist and its decimation filter removes them.
constexpr float saturationSplitHz = 4000.0f;

float decibelsToGain(float decibels) noexcept
{
    // ln(10)/20, folded. This is called several times per sample by the
    // compressor, so the difference between exp and pow is measurable.
    return std::exp(decibels * 0.11512925f);
}

float gainToDecibels(float gain, float floor = -100.0f) noexcept
{
    return gain > 0.0f ? std::max(floor, 20.0f * std::log10(gain)) : floor;
}
}

void ProcessingEngine::prepare(double newSampleRate, int maximumBlockSize, int channels)
{
    sampleRate.store(std::max(8000.0, newSampleRate), std::memory_order_release);
    preparedBlockSize = std::max(1, maximumBlockSize);
    preparedChannels = std::clamp(channels, 1, maximumChannels);
    // The lookahead must stay longer than the true-peak detector latency,
    // otherwise the limiter would react after the peak it is protecting from.
    lookaheadSamples = std::clamp(static_cast<int>(std::ceil(sampleRate * 0.001)),
                                  TruePeakDetector::latencySamples * 2,
                                  maximumLookaheadSamples - 1);
    // initProcessing sizes the oversampler's internal buffers and settles the
    // fractional-delay compensation, so the latency read below is the integer
    // one the delay lines have to span.
    oversampling.initProcessing(static_cast<size_t>(preparedBlockSize));
    oversamplerLatencySamples = static_cast<int>(std::llround(oversampling.getLatencyInSamples()));
    oversamplerLatencySamples = std::max(0, oversamplerLatencySamples);
    lineLength = std::min(lookaheadSamples + oversamplerLatencySamples, maximumLookaheadSamples - 1);
    // The oversampler's latency is already inside the zero-indexed block it
    // hands back, so the wet lane and the dry lane both present their sample
    // one `lineLength` after it arrived; taking `oversamplerLatencySamples`
    // off here would make the wet route arrive early by exactly that amount.
    wetDelayLength = lineLength;
    dryBuffer.setSize(preparedChannels, preparedBlockSize, false, true, false);
    preSaturationBuffer.setSize(maximumChannels, preparedBlockSize, false, true, false);

    const juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(preparedBlockSize),
                                         static_cast<juce::uint32>(preparedChannels) };
    for (auto* filter : { &middleSplit, &lowSplit, &highSplit, &lowGroupPhase, &highGroupPhase })
        filter->prepare(spec);
    middleSplit.setCutoffFrequency(500.0f);
    lowSplit.setCutoffFrequency(120.0f);
    highSplit.setCutoffFrequency(saturationSplitHz);
    lowGroupPhase.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    lowGroupPhase.setCutoffFrequency(saturationSplitHz);
    highGroupPhase.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    highGroupPhase.setCutoffFrequency(120.0f);

    for (auto* value : { &rumbleCutoff, &warmthGain, &lowGain, &mudGain, &clarityGain,
                         &harshGain, &sibilanceGain, &highGain, &outputGain })
        value->reset(sampleRate, 0.75);
    configureDynamicBands();
    bodyFilters[0].setPeak(sampleRate, bodyCentreHz, bodyCentreQ, bodyCentreGainDb);
    bodyFilters[1].setLowPass(sampleRate, bodyLowPassHz);
    // The music weight already ramps over seconds; this only keeps an operator
    // moving the slider from zippering.
    bodyMix.reset(sampleRate, 0.5);
    bodyMix.setCurrentAndTargetValue(0.0f);
    broadbandReferenceCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * broadbandReferenceSeconds));
    makeupCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * compressorMakeupSeconds));
    // A true DC blocker: about 5 Hz, and derived from the sample rate rather
    // than left as a constant that silently moves with it.
    dcBlockerCoefficient = static_cast<float>(std::clamp(
        1.0 - 2.0 * std::numbers::pi * 5.0 / sampleRate, 0.9, 0.99999));
    wetMix.reset(sampleRate, 0.05);
    dryMatchGain.reset(sampleRate, 0.25);
    // ~1.5 s integration: slow enough to track programme level instead of
    // individual syllables, fast enough to settle before the operator judges.
    loudnessMatchCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * 1.5));
    levelTracker.prepare(sampleRate);
    toneMatch.prepare(sampleRate);
    monoSpread.prepare(sampleRate);
    compressionOutputWeighting.prepare(sampleRate);
    compressionAverageCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * compressionCompensationSeconds));
    compensationSmoothing = std::exp(-1.0f / static_cast<float>(sampleRate * compressionCompensationSmoothingSeconds));
    stereoWidth.reset(sampleRate, 5.0);
    stereoBalance.reset(sampleRate, 5.0);
    // Slower than the Smart Engine width ramp. Correlation moves with every
    // chord change; the image must not.
    coherenceWidth.reset(sampleRate, 3.0);
    correlationCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * correlationIntegrationSeconds));

    rumbleCutoff.setCurrentAndTargetValue(20.0f);
    warmthGain.setCurrentAndTargetValue(0.0f);
    lowGain.setCurrentAndTargetValue(0.0f);
    mudGain.setCurrentAndTargetValue(0.0f);
    clarityGain.setCurrentAndTargetValue(0.0f);
    harshGain.setCurrentAndTargetValue(0.0f);
    sibilanceGain.setCurrentAndTargetValue(0.0f);
    highGain.setCurrentAndTargetValue(0.0f);
    outputGain.setCurrentAndTargetValue(1.0f);
    wetMix.setCurrentAndTargetValue(1.0f);
    stereoWidth.setCurrentAndTargetValue(1.0f);
    stereoBalance.setCurrentAndTargetValue(0.0f);
    coherenceWidth.setCurrentAndTargetValue(1.0f);
    dryMatchGain.setCurrentAndTargetValue(1.0f);
    // A fifth of the lookahead: long enough that the gain no longer steps, short
    // enough that it has settled onto the window minimum well before the peak
    // that minimum was computed for arrives at the output.
    limiterSmoothingCoefficient = std::exp(-5.0f / static_cast<float>(std::max(1, lineLength)));
    failsafeIncrement = static_cast<float>(1.0 / std::max(1.0, sampleRate * failsafeCrossfadeSeconds));
    failsafeHoldLength = std::max(1, static_cast<int>(sampleRate * failsafeHoldSeconds));
    reset();
    configureFilters();
}

void ProcessingEngine::resetProcessingState() noexcept
{
    for (auto& channel : wetDelay)
        channel.fill(0.0f);
    truePeakDetector.reset();
    for (auto& detector : multibandTruePeakDetectors)
        detector.reset();
    oversampling.reset();
    preSaturationBuffer.clear();
    dcPreviousInput.fill(0.0f);
    dcPreviousOutput.fill(0.0f);
    for (auto& filter : rumbleFilters)
        filter.reset();
    toneMatch.reset();
    monoSpread.reset();
    warmthFilter.reset();
    lowFilter.reset();
    mudFilter.reset();
    clarityFilter.reset();
    harshFilter.reset();
    sibilanceFilter.reset();
    highFilter.reset();
    for (auto& filter : sideBassFilters)
        filter.reset();
    for (auto& filter : bodyFilters)
        filter.reset();
    bodyEnvelopeSquare = 0.0f;
    bodyGain = 1.0f;
    for (auto& band : dynamicBands)
        band.reset();
    broadbandReferenceSquare = 0.0f;
    correlationLeftRight = 0.0;
    correlationLeftSquare = 0.0;
    correlationRightSquare = 0.0;
    measuredCorrelation = 1.0f;
    coherenceWidth.setCurrentAndTargetValue(1.0f);
    middleSplit.reset();
    lowSplit.reset();
    highSplit.reset();
    lowGroupPhase.reset();
    highGroupPhase.reset();
    bandEnvelopeSquare.fill(0.0f);
    bandGain.fill(1.0f);
    bandAverageGain.fill(1.0f);
    limiterGain = 1.0f;
    limiterGainHistory.fill(1.0f);
    limiterHistoryPosition = 0;
    limiterWindowMinimum = 1.0f;
    limiterSmoothedGain = 1.0f;
    multibandLimiterGain.fill(1.0f);
    lastMultibandLimiterReduction.fill(0.0f);
    lastCompressorReduction = 0.0f;
    lastLimiterReduction = 0.0f;
    levelTracker.reset();
    levelTotalDb = 0.0f;
    levelInputDb = 0.0f;
    inputLevelGain = 1.0f;
    outputLevelGain = 1.0f;
    compressionOutputWeighting.reset();
    compressionInputSquare = 0.0f;
    compressionOutputSquare = 0.0f;
    compensationDb = 0.0f;
    dryLoudnessSquare = 0.0;
    wetLoudnessSquare = 0.0;
    dryMatchGain.setCurrentAndTargetValue(1.0f);
}

void ProcessingEngine::reset() noexcept
{
    resetProcessingState();
    for (auto& channel : dryDelay)
        channel.fill(0.0f);
    wetDelayWritePosition = 0;
    dryDelayWritePosition = 0;
    failsafeBlend = 0.0f;
    failsafeHoldSamples = 0;
    failsafeStateCleared = true;
    metrics.failsafeActive.store(false, std::memory_order_relaxed);
    metrics.compressorGainReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.limiterGainReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.multibandLimiterLowReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.multibandLimiterMidReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.multibandLimiterHighReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.truePeakEstimate.store(0.0f, std::memory_order_relaxed);
    metrics.abMatchGainDb.store(0.0f, std::memory_order_relaxed);
    metrics.broadcastLevelGainDb.store(0.0f, std::memory_order_relaxed);
}

void ProcessingEngine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    if (channels == nullptr || numSamples <= 0 || numSamples > preparedBlockSize || preparedChannels <= 0)
        return;

    const auto activeChannels = std::min({ numChannels, preparedChannels, maximumChannels });
    if (activeChannels <= 0)
        return;

    updateTargets(numSamples);
    configureFilters();

    const auto punch = clampControl(parameters.punch.load(std::memory_order_relaxed));
    const auto dynamics = clampControl(parameters.dynamics.load(std::memory_order_relaxed));
    const auto warmth = clampControl(parameters.warmth.load(std::memory_order_relaxed));
    const auto body = clampControl(parameters.body.load(std::memory_order_relaxed))
        * std::clamp(adaptiveTargets.musicWeight.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const auto eqEnabled = parameters.adaptiveEqEnabled.load(std::memory_order_relaxed);
    const auto rumbleIsEnabled = parameters.rumbleEnabled.load(std::memory_order_relaxed);
    const auto toneMatchIsActive = parameters.toneMatchEnabled.load(std::memory_order_relaxed)
        && parameters.operatingMode.load(std::memory_order_relaxed) != static_cast<int>(OperatingMode::manual);
    const auto compressorIsEnabled = parameters.compressorEnabled.load(std::memory_order_relaxed);
    const auto saturationIsEnabled = parameters.saturationEnabled.load(std::memory_order_relaxed);
    const auto dynamicEqIsEnabled = eqEnabled
        && parameters.dynamicEqEnabled.load(std::memory_order_relaxed);
    const auto deEsserIsEnabled = eqEnabled
        && parameters.deEsserEnabled.load(std::memory_order_relaxed);
    const auto limiterIsEnabled = parameters.limiterEnabled.load(std::memory_order_relaxed);
    const auto multibandLimiterIsEnabled = parameters.multibandLimiterEnabled.load(std::memory_order_relaxed);
    const std::array<bool, multibandLimiterBandCount> multibandLimiterEnabled {
        parameters.multibandLimiterLowEnabled.load(std::memory_order_relaxed),
        parameters.multibandLimiterMidEnabled.load(std::memory_order_relaxed),
        parameters.multibandLimiterHighEnabled.load(std::memory_order_relaxed)
    };
    const std::array<float, multibandLimiterBandCount> multibandLimiterCeiling {
        std::clamp(parameters.multibandLimiterLowCeiling.load(std::memory_order_relaxed), multibandMinimumCeiling, 1.5f),
        std::clamp(parameters.multibandLimiterMidCeiling.load(std::memory_order_relaxed), multibandMinimumCeiling, 1.5f),
        std::clamp(parameters.multibandLimiterHighCeiling.load(std::memory_order_relaxed), multibandMinimumCeiling, 1.5f)
    };
    const std::array<float, multibandLimiterBandCount> multibandLimiterAttackMs {
        std::clamp(parameters.multibandLimiterLowAttackMs.load(std::memory_order_relaxed), 0.1f, 10.0f),
        std::clamp(parameters.multibandLimiterMidAttackMs.load(std::memory_order_relaxed), 0.1f, 10.0f),
        std::clamp(parameters.multibandLimiterHighAttackMs.load(std::memory_order_relaxed), 0.1f, 10.0f)
    };
    const std::array<float, multibandLimiterBandCount> multibandLimiterReleaseMs {
        std::clamp(parameters.multibandLimiterLowReleaseMs.load(std::memory_order_relaxed), 10.0f, 1000.0f),
        std::clamp(parameters.multibandLimiterMidReleaseMs.load(std::memory_order_relaxed), 10.0f, 1000.0f),
        std::clamp(parameters.multibandLimiterHighReleaseMs.load(std::memory_order_relaxed), 10.0f, 1000.0f)
    };
    const auto levelerIsEnabled = parameters.broadcastLevelerEnabled.load(std::memory_order_relaxed);
    // One setpoint, the operator's. Clamped only to keep an absurd stored value
    // out of the gain path.
    const auto levelerTargetDb = std::clamp(parameters.loudnessTarget.load(std::memory_order_relaxed),
                                            -30.0f, -8.0f);
    const auto failsafeForced = parameters.forceFailsafe.load(std::memory_order_relaxed);
    const auto monoSpreadIsEnabled = parameters.monoSpreadEnabled.load(std::memory_order_relaxed);
    const auto monoCompatibility = parameters.monoCompatibilityEnabled.load(std::memory_order_relaxed)
        && activeChannels == 2;
    if (monoCompatibility)
    {
        const auto corner = std::clamp(parameters.bassMonoFrequencyHz.load(std::memory_order_relaxed),
                                       40.0f, 300.0f);
        for (auto& filter : sideBassFilters)
            filter.setHighPass(sampleRate, corner);
    }

    // Correlation is integrated per sample but only resolved once per block:
    // the ratio needs a square root and a division, and the width it drives
    // ramps over seconds anyway.
    if (activeChannels == 2 && parameters.phaseCoherenceEnabled.load(std::memory_order_relaxed))
    {
        const auto denominator = std::sqrt(correlationLeftSquare * correlationRightSquare);
        measuredCorrelation = denominator > 1.0e-12
            ? std::clamp(static_cast<float>(correlationLeftRight / denominator), -1.0f, 1.0f)
            : 1.0f;
        const auto span = coherenceOnsetCorrelation - coherenceWorstCorrelation;
        const auto severity = std::clamp((coherenceOnsetCorrelation - measuredCorrelation) / span, 0.0f, 1.0f);
        coherenceWidth.setTargetValue(1.0f - severity * (1.0f - coherenceMinimumWidth));
    }
    else
    {
        measuredCorrelation = 1.0f;
        coherenceWidth.setTargetValue(1.0f);
    }

    const auto compressorThreshold = compressorThresholdAtMinimumDb - dynamics * compressorThresholdRangeDb
        + adaptiveTargets.compressionDb.load(std::memory_order_relaxed);
    const auto compressorRatio = compressorRatioAtMinimum + dynamics * compressorRatioRange;
    const std::array<float, 4> bandThresholdOffset { 2.0f, 0.0f, -1.0f, -2.0f };
    const std::array<float, 4> baseAttackMs { 35.0f, 28.0f, 22.0f, 15.0f };
    const std::array<float, 4> releaseMs { 190.0f, 165.0f, 135.0f, 110.0f };
    std::array<float, 4> envelopeAttack {};
    std::array<float, 4> envelopeRelease {};
    for (size_t band = 0; band < envelopeAttack.size(); ++band)
    {
        envelopeAttack[band] = std::exp(-1.0f / static_cast<float>(sampleRate * (baseAttackMs[band] + punch * 28.0f) * 0.001));
        envelopeRelease[band] = std::exp(-1.0f / static_cast<float>(sampleRate * releaseMs[band] * 0.001));
    }
    const auto gainAttack = std::exp(-1.0f / static_cast<float>(sampleRate * 0.008));
    const auto gainRelease = std::exp(-1.0f / static_cast<float>(sampleRate * 0.140));
    const auto saturationDrive = 1.0f + warmth * 0.16f;
    const auto saturationNormaliser = 1.0f / std::tanh(saturationDrive);
    const auto bodyAttack = std::exp(-1.0f / static_cast<float>(sampleRate * bodyAttackSeconds));
    const auto bodyRelease = std::exp(-1.0f / static_cast<float>(sampleRate * bodyReleaseSeconds));
    const auto limiterRelease = std::exp(-1.0f / static_cast<float>(sampleRate * 0.080));
    std::array<float, multibandLimiterBandCount> multibandLimiterAttack {};
    std::array<float, multibandLimiterBandCount> multibandLimiterRelease {};
    for (size_t band = 0; band < multibandLimiterBandCount; ++band)
    {
        multibandLimiterAttack[band] = std::exp(-1.0f / static_cast<float>(sampleRate * multibandLimiterAttackMs[band] * 0.001f));
        multibandLimiterRelease[band] = std::exp(-1.0f / static_cast<float>(sampleRate * multibandLimiterReleaseMs[band] * 0.001f));
    }
    // The leveler gain still ramps, but only enough to stop zipper noise: the
    // Kalman estimate it follows is already smooth by construction.
    const auto levelGainSmoothing = std::exp(-1.0f / static_cast<float>(sampleRate * 0.020));

    auto blockMaxCompressorReduction = 0.0f;
    auto blockMaxMakeup = 1.0f;
    auto blockMaxDeEsser = 0.0f;
    auto blockMaxDynamicEq = 0.0f;
    auto blockMaxLimiterReduction = 0.0f;
    std::array<float, multibandLimiterBandCount> blockMaxMultibandLimiterReduction {};
    auto blockTruePeak = 0.0f;
    const auto processedSelected = !parameters.bypass.load(std::memory_order_relaxed)
        && parameters.abProcessed.load(std::memory_order_relaxed);
    wetMix.setTargetValue(processedSelected ? 1.0f : 0.0f);
    bodyMix.setTargetValue(body * bodyMaximumMix);
    auto blockMaxBodyMix = 0.0f;
    const auto levelerActive = levelerIsEnabled && processedSelected;

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        float original[maximumChannels] { 0.0f, 0.0f };
        float toned[maximumChannels] { 0.0f, 0.0f };
        float bands[maximumChannels][4] {};
        float dynamicSignal[dynamicBandCount][maximumChannels] {};
        float dynamicDetector[dynamicBandCount] {};

        // The oversampler always runs its full channel count, so the scratch
        // channels this block is not writing keep their previous content. Zero
        // them so a mono block cannot feed stale buffer into the filters.
        for (int channel = activeChannels; channel < maximumChannels; ++channel)
        {
            dryBuffer.setSample(channel, sampleIndex, 0.0f);
            preSaturationBuffer.setSample(channel, sampleIndex, 0.0f);
        }

        // Static tone shaping, and extraction of the dynamic bands from the
        // result. Split from the crossover below because a dynamic band has to
        // see every channel before it can decide a gain: deciding per channel
        // would move the stereo image every time one side was louder.
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            auto input = channels[channel][sampleIndex];
            if (!std::isfinite(input))
            {
                input = 0.0f;
                ++nonFiniteInputCount;
            }
            // Clamped before the DC blocker: every stage after this one is
            // recursive, so a single absurd sample would otherwise stay in the
            // filter state long after the driver recovered.
            original[channel] = std::clamp(input, -inputMagnitudeClamp, inputMagnitudeClamp);
            dryBuffer.setSample(channel, sampleIndex, original[channel]);
            auto value = processDcBlocker(channel, original[channel]);
            if (rumbleIsEnabled)
                for (auto& filter : rumbleFilters)
                    value = filter.process(channel, value);
            if (eqEnabled)
            {
                value = warmthFilter.process(channel, value);
                value = lowFilter.process(channel, value);
                value = mudFilter.process(channel, value);
                value = clarityFilter.process(channel, value);
                value = harshFilter.process(channel, value);
                value = sibilanceFilter.process(channel, value);
                value = highFilter.process(channel, value);
            }
            toned[channel] = value;
            for (int band = 0; band < dynamicBandCount; ++band)
            {
                const auto extracted = dynamicBands[static_cast<size_t>(band)].extract(channel, value);
                dynamicSignal[band][channel] = extracted;
                dynamicDetector[band] = std::max(dynamicDetector[band], std::abs(extracted));
            }
        }

        auto tonedPower = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
            tonedPower += toned[channel] * toned[channel];
        tonedPower /= static_cast<float>(activeChannels);
        broadbandReferenceSquare = broadbandReferenceCoefficient * broadbandReferenceSquare
            + (1.0f - broadbandReferenceCoefficient) * tonedPower;
        const auto broadbandReferenceDb = gainToDecibels(std::sqrt(broadbandReferenceSquare), -180.0f);

        // Read before updating: the de-esser judges the sibilant against the
        // top end as it was, not against a running average the sibilant has
        // already been folded into.
        const auto sibilanceReferenceDb = dynamicBands[dynamicSibilance].getAverageLevelDb();
        dynamicBands[dynamicMud].updateGain(dynamicDetector[dynamicMud], broadbandReferenceDb, dynamicEqIsEnabled);
        dynamicBands[dynamicHarsh].updateGain(dynamicDetector[dynamicHarsh], broadbandReferenceDb, dynamicEqIsEnabled);
        dynamicBands[dynamicSibilance].updateGain(dynamicDetector[dynamicSibilance], sibilanceReferenceDb, deEsserIsEnabled);

        float levelled[maximumChannels] { 0.0f, 0.0f };
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            auto value = toned[channel];
            for (int band = 0; band < dynamicBandCount; ++band)
                value += (dynamicBands[static_cast<size_t>(band)].getGain() - 1.0f)
                    * dynamicSignal[band][channel];
            levelled[channel] = value;
        }

        // The leveller measures here, ahead of the compressor, and applies the
        // share of its gain that brings the programme to the working level.
        // Silence walks the estimate back to the target, which is what returns
        // the total gain to unity instead of leaving it armed for the next sound.
        levelTracker.process(levelled, activeChannels, levelerTargetDb);
        if (levelerActive)
        {
            // Recovery is capped so this cannot turn room noise into a
            // programme even if the gate is fooled. A closed gate holds both
            // gains where they were.
            //
            // The input share always aims at the working level, and when the
            // total runs into the cap the shortfall comes off the output share.
            // Taking it off the input instead left a very quiet sermon below
            // every compressor threshold, so the one programme that most needed
            // compressing got none.
            if (levelTracker.isTracking())
            {
                levelTotalDb = std::clamp(levelerTargetDb - levelTracker.getEstimateDb(),
                                          levelerMaximumCutDb, levelerMaximumBoostDb);
                levelInputDb = std::clamp(levelerWorkingLevelDb - levelTracker.getEstimateDb(),
                                          levelerMaximumCutDb - (levelerTargetDb - levelerWorkingLevelDb),
                                          levelerMaximumBoostDb);
            }
        }
        else
        {
            levelTotalDb = 0.0f;
            levelInputDb = 0.0f;
        }
        const auto inputTargetGain = decibelsToGain(levelInputDb);
        inputLevelGain = levelGainSmoothing * inputLevelGain + (1.0f - levelGainSmoothing) * inputTargetGain;

        for (int channel = 0; channel < activeChannels; ++channel)
        {
            float lowGroup = 0.0f, highGroup = 0.0f;
            middleSplit.processSample(channel, levelled[channel] * inputLevelGain, lowGroup, highGroup);
            lowSplit.processSample(channel, lowGroup, bands[channel][0], bands[channel][1]);
            highSplit.processSample(channel, highGroup, bands[channel][2], bands[channel][3]);
        }

        std::array<float, 4> appliedBandGain { 1.0f, 1.0f, 1.0f, 1.0f };
        for (size_t band = 0; band < bandGain.size(); ++band)
        {
            auto power = bands[0][band] * bands[0][band];
            if (activeChannels > 1) power = std::max(power, bands[1][band] * bands[1][band]);
            const auto envelopeCoefficient = power > bandEnvelopeSquare[band]
                ? envelopeAttack[band] : envelopeRelease[band];
            bandEnvelopeSquare[band] = envelopeCoefficient * bandEnvelopeSquare[band]
                + (1.0f - envelopeCoefficient) * power;
            auto targetGain = 1.0f;
            if (compressorIsEnabled)
            {
                const auto inputDb = gainToDecibels(std::sqrt(bandEnvelopeSquare[band]));
                const auto threshold = compressorThreshold + bandThresholdOffset[band];
                const auto over = inputDb - threshold;
                // Quadratic knee. Speech sits on the threshold for most of a
                // sentence, and a hard knee switches it in and out of
                // compression on every syllable, which is the breathing an
                // operator hears and blames on the ratio.
                auto reductionDb = 0.0f;
                if (over >= compressorKneeDb * 0.5f)
                    reductionDb = over - over / compressorRatio;
                else if (over > -compressorKneeDb * 0.5f)
                {
                    const auto knee = over + compressorKneeDb * 0.5f;
                    reductionDb = (1.0f - 1.0f / compressorRatio) * knee * knee
                        / (2.0f * compressorKneeDb);
                }
                targetGain = decibelsToGain(-reductionDb);
            }
            const auto coefficient = targetGain < bandGain[band] ? gainAttack : gainRelease;
            bandGain[band] = coefficient * bandGain[band] + (1.0f - coefficient) * targetGain;
            bandAverageGain[band] = makeupCoefficient * bandAverageGain[band]
                + (1.0f - makeupCoefficient) * bandGain[band];
            // Makeup interpolates between no compensation and fully undoing the
            // average reduction. Sustained reduction is given back, so a loud
            // passage gets denser instead of quieter; the fast movement the
            // compressor is actually there for is left alone. The detector runs
            // on the uncompressed band above, so none of this feeds back.
            const auto makeup = compressorIsEnabled
                ? 1.0f + (1.0f / std::max(bandAverageGain[band], 0.05f) - 1.0f) * compressorMakeupAmount
                : 1.0f;
            appliedBandGain[band] = bandGain[band] * makeup;
            blockMaxCompressorReduction = std::max(blockMaxCompressorReduction,
                                                    std::max(0.0f, -gainToDecibels(bandGain[band], -60.0f)));
            blockMaxMakeup = std::max(blockMaxMakeup, makeup);
        }

        // Reuse the compressor crossover tree as three limiter groups. The
        // low group intentionally shares one gain across its two constituent
        // bands so a 120 Hz crossover boundary cannot move with gain.
        if (multibandLimiterIsEnabled)
        {
            std::array<float, multibandLimiterBandCount> multibandGroupPeak {};
            for (int channel = 0; channel < activeChannels; ++channel)
            {
                const std::array<float, multibandLimiterBandCount> groups {
                    bands[channel][0] * appliedBandGain[0] + bands[channel][1] * appliedBandGain[1],
                    bands[channel][2] * appliedBandGain[2],
                    bands[channel][3] * appliedBandGain[3]
                };
                for (size_t band = 0; band < multibandLimiterBandCount; ++band)
                {
                    // The immediate sample peak removes the detector's FIR delay
                    // from the attack decision; the 4x detector catches the
                    // inter-sample interval once its history is available.
                    const auto detected = std::max(std::abs(groups[band]),
                        multibandTruePeakDetectors[band].process(channel, groups[band]));
                    multibandGroupPeak[band] = std::max(multibandGroupPeak[band], detected);
                }
            }
            for (size_t band = 0; band < multibandLimiterBandCount; ++band)
            {
                const auto target = multibandLimiterEnabled[band]
                    ? std::min(1.0f, multibandLimiterCeiling[band] / std::max(multibandGroupPeak[band], 1.0e-9f))
                    : 1.0f;
                const auto coefficient = target < multibandLimiterGain[band]
                    ? multibandLimiterAttack[band] : multibandLimiterRelease[band];
                multibandLimiterGain[band] = coefficient * multibandLimiterGain[band]
                    + (1.0f - coefficient) * target;
                blockMaxMultibandLimiterReduction[band] = std::max(blockMaxMultibandLimiterReduction[band],
                    std::max(0.0f, -gainToDecibels(multibandLimiterGain[band], -60.0f)));
            }
        }
        else
        {
            // Do not touch detector or gain state while bypassed. This keeps
            // the legacy path bit-for-bit identical and makes enable a clean
            // opt-in rather than an always-running hidden processor.
            multibandLimiterGain.fill(1.0f);
        }

        const auto blockOutputGain = outputGain.getNextValue();
        float recombined[maximumChannels] { 0.0f, 0.0f };
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto compressedLowGroup = bands[channel][0] * appliedBandGain[0]
                + bands[channel][1] * appliedBandGain[1];
            const auto compressedMid = bands[channel][2] * appliedBandGain[2];
            const auto compressedTop = bands[channel][3] * appliedBandGain[3];
            // The whole recombined band is saturated, not just low and mid. The
            // split used to exist so the top end stayed linear below Nyquist;
            // the 4x oversampler now owns that job, and saturating the complete
            // wet path is what actually sounds like a console drive stage on a
            // full mix instead of only its bottom two thirds.
            const auto value = lowGroupPhase.processSample(channel,
                                                             compressedLowGroup * multibandLimiterGain[0])
                + highGroupPhase.processSample(channel,
                                                compressedMid * multibandLimiterGain[1]
                                                + compressedTop * multibandLimiterGain[2]);
            recombined[channel] = value;
        }

        // The bus runs even at zero mix so its filters and detector are settled
        // the moment the operator, or the music weight, brings it in.
        const auto bodyAmount = bodyMix.getNextValue();
        float bodySignal[maximumChannels] { 0.0f, 0.0f };
        auto bodyPower = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            auto shaped = recombined[channel];
            for (auto& filter : bodyFilters)
                shaped = filter.process(channel, shaped);
            bodySignal[channel] = shaped;
            bodyPower = std::max(bodyPower, shaped * shaped);
        }
        const auto bodyCoefficient = bodyPower > bodyEnvelopeSquare ? bodyAttack : bodyRelease;
        bodyEnvelopeSquare = bodyCoefficient * bodyEnvelopeSquare + (1.0f - bodyCoefficient) * bodyPower;
        const auto bodyLevelDb = gainToDecibels(std::sqrt(bodyEnvelopeSquare));
        const auto bodyOver = std::max(0.0f, bodyLevelDb - bodyThresholdDb);
        const auto bodyUnder = std::max(0.0f, bodyFloorDb - bodyLevelDb);
        const auto bodyTargetGain = decibelsToGain(-std::min(bodyOver - bodyOver / bodyRatio
                                                                 + bodyUnder * bodyExpansionRatio,
                                                             bodyMaximumReductionDb));
        bodyGain = (bodyTargetGain < bodyGain ? gainAttack : gainRelease) * (bodyGain - bodyTargetGain) + bodyTargetGain;
        blockMaxBodyMix = std::max(blockMaxBodyMix, bodyAmount);
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto value = recombined[channel] + bodySignal[channel] * bodyGain * bodyAmount;
            preSaturationBuffer.setSample(channel, sampleIndex, value * blockOutputGain);
        }
    }

    // Oversampled saturation, applied as one block between the two per-sample
    // loops. The tanh itself runs at 4x the stream rate on the block the
    // oversampler hands back, then the result is decimated back down. Running
    // the pass even with saturation disabled keeps the latency, and therefore
    // the dry/wet alignment, constant while the operator toggles it.
    {
        auto block = juce::dsp::AudioBlock<float>(preSaturationBuffer)
            .getSubBlock(0, static_cast<size_t>(numSamples));
        const auto upsampled = oversampling.processSamplesUp(block);
        if (saturationIsEnabled)
        {
            const auto upsampledSamples = static_cast<int>(upsampled.getNumSamples());
            for (int ch = 0; ch < maximumChannels; ++ch)
            {
                auto* samples = upsampled.getChannelPointer(static_cast<size_t>(ch));
                for (int i = 0; i < upsampledSamples; ++i)
                    samples[i] = std::tanh(samples[i] * saturationDrive) * saturationNormaliser;
            }
        }
        oversampling.processSamplesDown(block);
    }

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        float wet[maximumChannels] { 0.0f, 0.0f };
        float original[maximumChannels] { 0.0f, 0.0f };
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            wet[channel] = preSaturationBuffer.getSample(channel, sampleIndex);
            original[channel] = dryBuffer.getSample(channel, sampleIndex);
        }

        // ToneMatch sits after the dynamics, the way mastering EQ does: in front
        // of a multiband compressor, a lift raises its band into more gain
        // reduction and the compressor's thresholds pull the balance back
        // towards their own. It measures what reaches it, before its own
        // filters, so the correction depends on the programme and never on
        // itself.
        toneMatch.process(wet, activeChannels);

        // Coherence safety never widens: it can only take back what the Smart
        // Engine asked for, so the two controls cannot fight each other.
        const auto width = stereoWidth.getNextValue();
        const auto balance = stereoBalance.getNextValue();
        const auto safeWidth = std::min(width, coherenceWidth.getNextValue());
        if (activeChannels == 2)
        {
            correlationLeftRight = correlationCoefficient * correlationLeftRight
                + (1.0f - correlationCoefficient) * static_cast<double>(wet[0]) * wet[1];
            correlationLeftSquare = correlationCoefficient * correlationLeftSquare
                + (1.0f - correlationCoefficient) * static_cast<double>(wet[0]) * wet[0];
            correlationRightSquare = correlationCoefficient * correlationRightSquare
                + (1.0f - correlationCoefficient) * static_cast<double>(wet[1]) * wet[1];
        }
        // Added to the Side, so the mono sum is untouched; and ahead of the
        // bass-mono filters and the width controls, so both still have the last
        // word over it. It runs every sample so its detector and all-passes are
        // settled whenever a mono feed appears.
        const auto spreadSide = activeChannels == 2
            ? monoSpread.process(original[0], original[1], 0.5f * (wet[0] + wet[1]), monoSpreadIsEnabled)
            : 0.0f;
        if (activeChannels == 2 && (monoCompatibility || safeWidth < 0.9999f || spreadSide != 0.0f))
        {
            const auto mid = 0.5f * (wet[0] + wet[1]);
            auto side = 0.5f * (wet[0] - wet[1]) + spreadSide;
            if (monoCompatibility)
                for (auto& filter : sideBassFilters)
                    side = filter.process(0, side);
            side *= safeWidth;
            wet[0] = mid + side;
            wet[1] = mid - side;
        }
        if (activeChannels == 2 && std::abs(balance) > 1.0e-5f)
        {
            wet[0] *= decibelsToGain(balance * 0.5f);
            wet[1] *= decibelsToGain(-balance * 0.5f);
        }

        // The rest of the leveller gain: working level to the operator's
        // target, plus whatever loudness the compression took. The input side
        // of the ratio is the same K-weighted power the tracker measured, with
        // the input gain applied, so the two averages describe the same
        // programme on either side of the compressor.
        auto outputPower = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto weighted = compressionOutputWeighting.process(channel, wet[channel]);
            outputPower += weighted * weighted;
        }
        if (levelTracker.isGateOpen())
        {
            const auto inputPower = levelTracker.getLastPower() * inputLevelGain * inputLevelGain;
            compressionInputSquare = compressionAverageCoefficient * compressionInputSquare
                + (1.0f - compressionAverageCoefficient) * inputPower;
            compressionOutputSquare = compressionAverageCoefficient * compressionOutputSquare
                + (1.0f - compressionAverageCoefficient) * outputPower;
        }
        auto compensationTargetDb = compensationDb;
        if (!levelerActive || levelTracker.isInSilence())
            compensationTargetDb = 0.0f;
        else if (levelTracker.isGateOpen() && compressionInputSquare > 1.0e-12f && compressionOutputSquare > 1.0e-12f)
            compensationTargetDb = std::clamp(10.0f * std::log10(compressionInputSquare / compressionOutputSquare),
                                              -compressionCompensationRangeDb, compressionCompensationRangeDb);
        compensationDb = compensationSmoothing * compensationDb + (1.0f - compensationSmoothing) * compensationTargetDb;
        const auto outputTargetGain = decibelsToGain(levelTotalDb - levelInputDb + compensationDb);
        outputLevelGain = levelGainSmoothing * outputLevelGain + (1.0f - levelGainSmoothing) * outputTargetGain;
        for (int channel = 0; channel < activeChannels; ++channel)
            wet[channel] *= outputLevelGain;

        auto detectedTruePeak = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
            detectedTruePeak = std::max(detectedTruePeak, truePeakDetector.process(channel, wet[channel]));
        blockTruePeak = std::max(blockTruePeak, detectedTruePeak);

        const auto targetLimiterGain = limiterIsEnabled
            ? std::min(1.0f, limiterCeiling / std::max(detectedTruePeak, 1.0e-9f))
            : 1.0f;
        if (targetLimiterGain < limiterGain)
            limiterGain = targetLimiterGain;
        else
            limiterGain = limiterRelease * limiterGain + (1.0f - limiterRelease);

        // The sample about to leave the wet delay line is one `wetDelayLength`
        // old, so the gain that protects it is somewhere in the window of gains
        // computed since. Taking the minimum of that window means the reduction
        // is already in place before the peak arrives and is held for as long as
        // the peak can still be in flight, which is what makes it safe to
        // smooth. Applying the instantaneous gain instead was a one-sample step,
        // and a step in gain is broadband energy: on dense programme the
        // detector fires constantly and it is audible as grit on the low end.
        const auto outgoing = limiterGainHistory[static_cast<size_t>(limiterHistoryPosition)];
        limiterGainHistory[static_cast<size_t>(limiterHistoryPosition)] = limiterGain;
        if (++limiterHistoryPosition >= wetDelayLength)
            limiterHistoryPosition = 0;
        if (limiterGain <= limiterWindowMinimum)
            limiterWindowMinimum = limiterGain;
        else if (outgoing <= limiterWindowMinimum)
            limiterWindowMinimum = *std::min_element(limiterGainHistory.begin(),
                                                     limiterGainHistory.begin() + wetDelayLength);
        limiterSmoothedGain = limiterSmoothingCoefficient * limiterSmoothedGain
            + (1.0f - limiterSmoothingCoefficient) * limiterWindowMinimum;
        const auto limiterReduction = std::max(0.0f, -gainToDecibels(limiterSmoothedGain, -60.0f));
        blockMaxLimiterReduction = std::max(blockMaxLimiterReduction, limiterReduction);
        blockMaxDeEsser = std::max(blockMaxDeEsser, dynamicBands[dynamicSibilance].getReductionDb());
        blockMaxDynamicEq = std::max(blockMaxDynamicEq,
                                     std::max(dynamicBands[dynamicMud].getReductionDb(),
                                              dynamicBands[dynamicHarsh].getReductionDb()));

        auto faulted = !std::isfinite(limiterGain) || !std::isfinite(limiterSmoothedGain);
        for (const auto gain : multibandLimiterGain)
            faulted = faulted || !std::isfinite(gain);
        for (int channel = 0; channel < activeChannels; ++channel)
            faulted = faulted || !std::isfinite(wet[channel])
                || std::abs(wet[channel]) > failsafeMagnitude;
        if (faulted)
        {
            if (failsafeHoldSamples == 0 && failsafeBlend <= 0.0f)
                ++failsafeEngagementCount;
            failsafeHoldSamples = failsafeHoldLength;
            failsafeStateCleared = false;
            // Never let the broken sample into the delay line: it would come
            // back out one lookahead later, after the crossfade had settled.
            for (int channel = 0; channel < activeChannels; ++channel)
                wet[channel] = 0.0f;
        }
        // A forced failsafe is an operator or supervisor decision, not a DSP
        // fault: it holds the dry path open without counting an engagement and
        // without discarding filter state that was never broken.
        if (failsafeForced)
            failsafeHoldSamples = std::max(failsafeHoldSamples, 1);

        const auto mix = wetMix.getNextValue();
        const auto matchGain = dryMatchGain.getNextValue();

        auto dryMono = 0.0f;
        auto wetMono = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto delayedWet = wetDelay[static_cast<size_t>(channel)][static_cast<size_t>(wetDelayWritePosition)];
            const auto delayedDry = dryDelay[static_cast<size_t>(channel)][static_cast<size_t>(dryDelayWritePosition)];
            wetDelay[static_cast<size_t>(channel)][static_cast<size_t>(wetDelayWritePosition)] = wet[channel];
            dryDelay[static_cast<size_t>(channel)][static_cast<size_t>(dryDelayWritePosition)] = original[channel];
            const auto limitedWet = delayedWet * limiterSmoothedGain;
            const auto matchedDry = delayedDry * matchGain;
            dryMono += delayedDry;
            wetMono += limitedWet;
            const auto processed = matchedDry + (limitedWet - matchedDry) * mix;
            // The safety path is the delayed dry signal, unmatched and
            // unprocessed: it stays sample aligned with the reported latency,
            // and like bypass it is never gain matched.
            channels[channel][sampleIndex] = processed + (delayedDry - processed) * failsafeBlend;
        }
        const auto monoScale = activeChannels > 1 ? 0.5f : 1.0f;
        updateLoudnessMatch(dryMono * monoScale, wetMono * monoScale);

        const auto failsafeTarget = failsafeHoldSamples > 0 ? 1.0f : 0.0f;
        if (failsafeBlend < failsafeTarget)
            failsafeBlend = std::min(failsafeTarget, failsafeBlend + failsafeIncrement);
        else if (failsafeBlend > failsafeTarget)
            failsafeBlend = std::max(failsafeTarget, failsafeBlend - failsafeIncrement);

        if (failsafeHoldSamples > 0)
        {
            --failsafeHoldSamples;
            // Clearing the poisoned state is only safe once the processed path
            // is fully muted, otherwise the reset itself becomes an audible
            // click on top of the fault it is repairing.
            if (!failsafeStateCleared && failsafeBlend >= 1.0f)
            {
                resetProcessingState();
                failsafeStateCleared = true;
            }
        }

        if (++wetDelayWritePosition >= wetDelayLength)
            wetDelayWritePosition = 0;
        if (++dryDelayWritePosition >= lineLength)
            dryDelayWritePosition = 0;
    }

    // Programme is judged by the leveller's gate, so a prayer pause or the room
    // between songs is not measured as a dull mix.
    toneMatch.endBlock(levelTracker.isGateOpen(), toneMatchIsActive);
    for (size_t control = 0; control < ToneMatch::controlCount; ++control)
        metrics.toneMatchGainDb[control].store(toneMatch.getGainsDb()[control], std::memory_order_release);

    lastCompressorReduction = std::max(blockMaxCompressorReduction, lastCompressorReduction * 0.92f);
    lastLimiterReduction = std::max(blockMaxLimiterReduction, lastLimiterReduction * 0.90f);
    for (size_t band = 0; band < multibandLimiterBandCount; ++band)
        lastMultibandLimiterReduction[band] = std::max(blockMaxMultibandLimiterReduction[band],
                                                       lastMultibandLimiterReduction[band] * 0.90f);
    metrics.compressorGainReductionDb.store(lastCompressorReduction, std::memory_order_release);
    metrics.limiterGainReductionDb.store(lastLimiterReduction, std::memory_order_release);
    metrics.multibandLimiterLowReductionDb.store(lastMultibandLimiterReduction[0], std::memory_order_release);
    metrics.multibandLimiterMidReductionDb.store(lastMultibandLimiterReduction[1], std::memory_order_release);
    metrics.multibandLimiterHighReductionDb.store(lastMultibandLimiterReduction[2], std::memory_order_release);
    metrics.truePeakEstimate.store(blockTruePeak, std::memory_order_release);
    metrics.appliedOutputGainDb.store(gainToDecibels(outputGain.getCurrentValue()), std::memory_order_release);
    metrics.deEsserReductionDb.store(blockMaxDeEsser, std::memory_order_release);
    metrics.dynamicEqReductionDb.store(blockMaxDynamicEq, std::memory_order_release);
    metrics.compressorMakeupDb.store(gainToDecibels(blockMaxMakeup, 0.0f), std::memory_order_release);
    metrics.bodyMix.store(blockMaxBodyMix / bodyMaximumMix, std::memory_order_release);
    metrics.broadcastLevelGainDb.store(gainToDecibels(inputLevelGain * outputLevelGain), std::memory_order_release);
    metrics.abMatchGainDb.store(gainToDecibels(dryMatchGain.getCurrentValue(), -24.0f), std::memory_order_release);
    metrics.failsafeActive.store(failsafeBlend > 0.0f, std::memory_order_release);
    metrics.failsafeEngagements.store(failsafeEngagementCount, std::memory_order_release);
    metrics.nonFiniteInputSamples.store(nonFiniteInputCount, std::memory_order_release);
    metrics.programmeCorrelation.store(measuredCorrelation, std::memory_order_release);
    metrics.monoSpreadWeight.store(monoSpread.getWeight(), std::memory_order_release);
    metrics.appliedStereoWidth.store(std::min(stereoWidth.getCurrentValue(), coherenceWidth.getCurrentValue()),
                                     std::memory_order_release);
    metrics.programmeLevelDb.store(levelTracker.getEstimateDb(), std::memory_order_release);
    metrics.levelerGateOpen.store(levelTracker.isGateOpen(), std::memory_order_release);
}

void ProcessingEngine::updateLoudnessMatch(float dryMono, float wetMono) noexcept
{
    dryLoudnessSquare = loudnessMatchCoefficient * dryLoudnessSquare
        + (1.0 - loudnessMatchCoefficient) * static_cast<double>(dryMono) * dryMono;
    wetLoudnessSquare = loudnessMatchCoefficient * wetLoudnessSquare
        + (1.0 - loudnessMatchCoefficient) * static_cast<double>(wetMono) * wetMono;
}

float ProcessingEngine::processDcBlocker(int channel, float sample) noexcept
{
    const auto index = static_cast<size_t>(channel);
    auto output = sample - dcPreviousInput[index] + dcBlockerCoefficient * dcPreviousOutput[index];
    if (!std::isfinite(output))
        output = 0.0f;
    dcPreviousInput[index] = sample;
    dcPreviousOutput[index] = output;
    return output;
}


void ProcessingEngine::updateTargets(int numSamples) noexcept
{
    const auto clean = clampControl(parameters.clean.load(std::memory_order_relaxed));
    const auto clarity = clampControl(parameters.clarity.load(std::memory_order_relaxed));
    const auto warmth = clampControl(parameters.warmth.load(std::memory_order_relaxed));
    const auto smart = parameters.smartProcessing.load(std::memory_order_relaxed)
        && parameters.operatingMode.load(std::memory_order_relaxed) != static_cast<int>(OperatingMode::manual);
    const auto autoMode = parameters.operatingMode.load(std::memory_order_relaxed)
        == static_cast<int>(OperatingMode::autoMode);

    // What actually dirties a church stream lives between 40 and 90 Hz, not at
    // 20: microphone handling, air conditioning, and people walking on a
    // hollow platform. The old default of 20 Hz with a second-order slope
    // removed essentially none of it.
    const auto adaptiveRumble = smart ? adaptiveTargets.rumbleCutoffHz.load(std::memory_order_relaxed) : 30.0f;
    // Squared rather than linear, because now that the slope is fourth order
    // the corner matters far more than it used to. A linear control puts its
    // midpoint near 50 Hz, which costs a low E on the bass about 4 dB; squared
    // keeps the middle of the range around 44 Hz where nothing musical is lost,
    // and still reaches 85 Hz for a stream that is only ever speech.
    rumbleCutoff.setTargetValue(std::clamp(std::max(30.0f + clean * clean * 55.0f, adaptiveRumble),
                                           30.0f, 90.0f));
    // Ranges that a listener can actually hear. At 0.8 dB the WARMTH control
    // was below the threshold of being noticed at all, and with every other
    // band limited to cutting, the engine could only ever take things away.
    warmthGain.setTargetValue(std::clamp(warmth * 2.5f, 0.0f, 2.5f));
    lowGain.setTargetValue(smart ? std::clamp(adaptiveTargets.lowGainDb.load(std::memory_order_relaxed), autoMode ? -2.0f : -1.5f, 0.5f) : 0.0f);
    mudGain.setTargetValue(std::clamp(-clean * 0.8f
                                         + (smart ? adaptiveTargets.mudGainDb.load(std::memory_order_relaxed) : 0.0f)
                                         + parameters.analystMudOffsetDb.load(std::memory_order_relaxed),
                                     autoMode ? -3.0f : -2.0f, 0.0f));
    clarityGain.setTargetValue(std::clamp(clarity * 1.8f
                                             + (smart ? adaptiveTargets.clarityGainDb.load(std::memory_order_relaxed) : 0.0f),
                                         -0.5f, autoMode ? 3.5f : 3.0f));
    harshGain.setTargetValue(std::clamp((smart ? adaptiveTargets.harshGainDb.load(std::memory_order_relaxed) : 0.0f)
                                        + parameters.analystHarshOffsetDb.load(std::memory_order_relaxed),
                                        autoMode ? -3.0f : -2.0f, 0.0f));
    sibilanceGain.setTargetValue(std::clamp((smart ? adaptiveTargets.sibilanceGainDb.load(std::memory_order_relaxed) : 0.0f)
                                            + parameters.analystSibilanceOffsetDb.load(std::memory_order_relaxed),
                                            autoMode ? -4.0f : -2.5f, 0.0f));
    // The air shelf carries both the user's CLARITY and the Smart Engine's
    // high-frequency correction, so a bright room can be smoothed and a dull
    // one can be opened up with the same control instead of only the former.
    highGain.setTargetValue(std::clamp(clarity * 1.2f
                                           + (smart ? adaptiveTargets.highGainDb.load(std::memory_order_relaxed) : 0.0f),
                                       autoMode ? -2.5f : -2.0f, autoMode ? 2.5f : 2.0f));
    // Only one loop may own the programme level. The leveller measures after
    // this gain, so when both are running the leveller simply takes back
    // whatever the Smart Engine adds and the two setpoints fight, leaving the
    // stream short of the operator's target with the gain slowly wandering.
    // The leveller is the better of the two at this: it is gated, K-weighted
    // and sample accurate, where this is a 2 to 10 Hz correction of a
    // short-term reading.
    const auto levelerOwnsLoudness = parameters.broadcastLevelerEnabled.load(std::memory_order_relaxed);
    const auto targetOutputDb = smart && !levelerOwnsLoudness
        ? std::clamp(adaptiveTargets.loudnessGainDb.load(std::memory_order_relaxed), autoMode ? -4.0f : -3.0f, autoMode ? 4.0f : 3.0f)
        : 0.0f;
    outputGain.setTargetValue(decibelsToGain(targetOutputDb));
    stereoWidth.setTargetValue(std::clamp(adaptiveTargets.stereoWidth.load(std::memory_order_relaxed), 0.80f, 1.0f));
    stereoBalance.setTargetValue(std::clamp(adaptiveTargets.stereoBalanceDb.load(std::memory_order_relaxed), -0.75f, 0.75f));

    rumbleCutoff.skip(numSamples);
    warmthGain.skip(numSamples);
    lowGain.skip(numSamples);
    mudGain.skip(numSamples);
    clarityGain.skip(numSamples);
    harshGain.skip(numSamples);
    sibilanceGain.skip(numSamples);
    highGain.skip(numSamples);

    // Loudness-matched A/B. Only the explicit comparison is matched; bypass
    // stays a literal bypass so the safety path is never altered.
    const auto matchRequested = !parameters.bypass.load(std::memory_order_relaxed)
        && !parameters.abProcessed.load(std::memory_order_relaxed)
        && parameters.abLoudnessMatch.load(std::memory_order_relaxed);
    auto matchTarget = 1.0f;
    if (matchRequested && dryLoudnessSquare > 1.0e-9 && wetLoudnessSquare > 1.0e-9)
    {
        // std::clamp propagates NaN rather than rejecting it, so the ratio is
        // checked before it can be latched into the smoother for good.
        const auto ratio = static_cast<float>(std::sqrt(wetLoudnessSquare / dryLoudnessSquare));
        if (std::isfinite(ratio))
            matchTarget = std::clamp(ratio, decibelsToGain(-12.0f), decibelsToGain(12.0f));
    }
    dryMatchGain.setTargetValue(matchTarget);
}

void ProcessingEngine::configureFilters() noexcept
{
    // The Q pair is what makes two cascaded biquads a maximally flat fourth
    // order Butterworth rather than a section that peaks at the corner.
    rumbleFilters[0].setHighPass(sampleRate, rumbleCutoff.getCurrentValue(), 0.54119610f);
    rumbleFilters[1].setHighPass(sampleRate, rumbleCutoff.getCurrentValue(), 1.30656296f);
    // A shelf rather than a peak at 110 Hz. A peak adds weight in one narrow
    // place, which on a stream heard mostly on phone speakers is the one place
    // that will not reproduce; a shelf adds the whole bottom together.
    warmthFilter.setLowShelf(sampleRate, 150.0f, 0.70f, warmthGain.getCurrentValue());
    lowFilter.setPeak(sampleRate, 90.0f, 0.85f, lowGain.getCurrentValue());
    mudFilter.setPeak(sampleRate, 240.0f, 0.80f, mudGain.getCurrentValue());
    // Tighter than before. At Q 0.75 the presence lift reached to 4 kHz and the
    // harshness cut reached down to 3.5 kHz, so the two fought over the same
    // octave and the Smart Engine needed an explicit rule to stop them.
    clarityFilter.setPeak(sampleRate, 2500.0f, 1.00f, clarityGain.getCurrentValue());
    harshFilter.setPeak(sampleRate, 5200.0f, 1.20f, harshGain.getCurrentValue());
    // Narrow enough to sit under sibilance without dulling the whole top end.
    sibilanceFilter.setPeak(sampleRate, 7400.0f, 3.20f, sibilanceGain.getCurrentValue());
    // Air, as a shelf. The 12 kHz peak it replaces had Q 0.70, which spans 7 to
    // 20 kHz and therefore sat directly on top of the de-esser, and it could
    // only ever cut: the whole tonal engine was subtractive, which is why the
    // result came out clean and lifeless.
    highFilter.setHighShelf(sampleRate, 9000.0f, 0.70f, highGain.getCurrentValue());
}

void ProcessingEngine::configureDynamicBands() noexcept
{
    dynamicBands[dynamicMud].prepare(sampleRate, dynamicMudCentreHz, dynamicMudQ,
                                     dynamicSustainedAttackSeconds, dynamicSustainedReleaseSeconds);
    dynamicBands[dynamicMud].setResponse(dynamicMudOffsetDb, dynamicMudRatio, dynamicMudMaximumDb);
    dynamicBands[dynamicHarsh].prepare(sampleRate, dynamicHarshCentreHz, dynamicHarshQ,
                                       dynamicSustainedAttackSeconds, dynamicSustainedReleaseSeconds);
    dynamicBands[dynamicHarsh].setResponse(dynamicHarshOffsetDb, dynamicHarshRatio, dynamicHarshMaximumDb);
    dynamicBands[dynamicSibilance].prepare(sampleRate, deEsserCentreHz, deEsserQ,
                                           deEsserAttackSeconds, deEsserReleaseSeconds);
    dynamicBands[dynamicSibilance].setResponse(deEsserOffsetDb, deEsserRatio, deEsserMaximumDb);
}

float ProcessingEngine::clampControl(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}
} // namespace churchstream
