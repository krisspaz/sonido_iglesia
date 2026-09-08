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
constexpr float levelerMaximumBoostDb = 15.0f;
constexpr float levelerMaximumCutDb = -10.0f;
// Kalman tuning, expressed as the time constant each steady-state gain
// corresponds to, because seconds are reviewable and covariances are not.
constexpr double kalmanSteadySeconds = 2.00;
constexpr double kalmanSectionChangeSeconds = 0.05;
constexpr double kalmanDetectorSeconds = 0.05;
constexpr double kalmanInnovationSeconds = 0.50;
// Measurement noise in dB^2. Programme RMS wanders by a few dB between
// syllables even when nothing about the mix has changed.
constexpr float kalmanMeasurementNoise = 9.0f;
// Innovation this small is ordinary programme variation; this large is a
// section change. Between them the filter speeds up proportionally.
constexpr float kalmanInnovationFloorDb = 4.0f;
constexpr float kalmanInnovationCeilingDb = 12.0f;
// Gating, in the shape of BS.1770: an absolute floor plus a relative gate
// below the running programme level. The relative gate does the real work of
// excluding prayer pauses; the absolute one only catches true silence.
constexpr double loudnessAverageSeconds = 30.0;
constexpr float levelerAbsoluteGateDb = -60.0f;
constexpr float levelerRelativeGateDb = 10.0f;
// A quieter section settles near the relative gate rather than far below it,
// and without hysteresis it sits there flickering the gate open and shut. That
// costs more than a few wasted branches: the hold that decides pause from
// section restarts on every flicker, so the section is never recognised at all.
constexpr float levelerGateHysteresisDb = 2.0f;
// How long silence must last before the leveler stands down, and how quickly
// it then slides to unity. Nothing is audible while it happens, by definition.
// Long enough to sit through a pause for prayer, short enough that a genuinely
// quieter section is picked up while it is still the same thought.
constexpr double levelerGateHoldSeconds = 2.50;
// How long the estimate is allowed to move quickly after a section has been
// accepted. Long enough to cover the step, short enough that the next fall is
// judged on its own merits.
constexpr double levelerSectionRebaseSeconds = 3.00;
constexpr double levelerGateIntegrationSeconds = 0.40;
constexpr double levelerVariationSeconds = 0.70;
// Speech modulates its own level by several dB just by being speech. Steady
// noise sits far below this even before the step that produced it has settled.
constexpr float levelerSpeechVariationDb = 2.0f;
constexpr double levelerSilenceReleaseSeconds = 1.50;
constexpr double levelerSilenceSlideSeconds = 0.60;
constexpr float kalmanInitialCovariance = 400.0f;

// Compressor. A hard knee with the programme sitting near the threshold is the
// classic source of breathing on speech, because every syllable crosses it.
constexpr float compressorKneeDb = 8.0f;
constexpr double compressorMakeupSeconds = 0.50;
// Not the full reduction. Returning all of it would undo the compression on
// anything sustained; returning most of it keeps the level while still letting
// the fast movement be controlled.
constexpr float compressorMakeupAmount = 0.80f;

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
    // One oversampler latency shorter, so the wet lane (which the oversampler
    // already delayed) and the dry lane both present their sample one
    // `lineLength` after it arrived.
    wetDelayLength = std::max(1, lineLength - oversamplerLatencySamples);
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
    levelerWeighting.prepare(sampleRate);
    configureDynamicBands();
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
    // A Kalman gain of 1/(fs*tau) behaves like a one-pole with that time
    // constant, and the steady-state relation K = sqrt(Q/R) inverts to give the
    // process noise the filter needs to settle there.
    kalmanSteadyGain = static_cast<float>(1.0 / (sampleRate * kalmanSteadySeconds));
    kalmanFastGain = static_cast<float>(1.0 / (sampleRate * kalmanSectionChangeSeconds));
    levelDetectorCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * kalmanDetectorSeconds));
    innovationCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * kalmanInnovationSeconds));
    loudnessAverageCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * loudnessAverageSeconds));
    gateHoldSamples = static_cast<float>(sampleRate * levelerGateHoldSeconds);
    sectionRebaseLength = static_cast<float>(sampleRate * levelerSectionRebaseSeconds);
    levelTrendCoefficient = static_cast<float>(1.0 / (sampleRate * levelerVariationSeconds));
    gateLevelCoefficient = static_cast<float>(1.0 / (sampleRate * levelerGateIntegrationSeconds));
    silenceReleaseSamples = static_cast<float>(sampleRate * levelerSilenceReleaseSeconds);
    silenceReleaseGain = static_cast<float>(1.0 / (sampleRate * levelerSilenceSlideSeconds));
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
    oversampling.reset();
    preSaturationBuffer.clear();
    dcPreviousInput.fill(0.0f);
    dcPreviousOutput.fill(0.0f);
    for (auto& filter : rumbleFilters)
        filter.reset();
    warmthFilter.reset();
    lowFilter.reset();
    mudFilter.reset();
    clarityFilter.reset();
    harshFilter.reset();
    sibilanceFilter.reset();
    highFilter.reset();
    for (auto& filter : sideBassFilters)
        filter.reset();
    for (auto& band : dynamicBands)
        band.reset();
    broadbandReferenceSquare = 0.0f;
    levelerWeighting.reset();
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
    lastCompressorReduction = 0.0f;
    lastLimiterReduction = 0.0f;
    programmeLevelSquare = 0.0f;
    programmeLevelGain = 1.0f;
    // A large starting covariance is what lets the estimate lock onto the first
    // real programme within a fraction of a second instead of creeping towards
    // it with the steady-state time constant.
    programmeLevelEstimateDb = -100.0f;
    programmeLevelCovariance = kalmanInitialCovariance;
    programmeInnovationAverage = 0.0f;
    programmeLevelInitialised = false;
    programmeLoudnessAverage = -100.0f;
    loudnessSampleCount = 0.0f;
    gateClosedSamples = 0.0f;
    sectionRebaseSamples = 0.0f;
    gateLevelDb = -100.0f;
    programmeLevelTrend = -100.0f;
    levelVariationDb = 0.0f;
    levelerGateOpen = false;
    silenceSamples = 0.0f;
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
    const auto eqEnabled = parameters.adaptiveEqEnabled.load(std::memory_order_relaxed);
    const auto rumbleIsEnabled = parameters.rumbleEnabled.load(std::memory_order_relaxed);
    const auto compressorIsEnabled = parameters.compressorEnabled.load(std::memory_order_relaxed);
    const auto saturationIsEnabled = parameters.saturationEnabled.load(std::memory_order_relaxed);
    const auto dynamicEqIsEnabled = eqEnabled
        && parameters.dynamicEqEnabled.load(std::memory_order_relaxed);
    const auto deEsserIsEnabled = eqEnabled
        && parameters.deEsserEnabled.load(std::memory_order_relaxed);
    const auto limiterIsEnabled = parameters.limiterEnabled.load(std::memory_order_relaxed);
    const auto levelerIsEnabled = parameters.broadcastLevelerEnabled.load(std::memory_order_relaxed);
    // One setpoint, the operator's. Clamped only to keep an absurd stored value
    // out of the gain path.
    const auto levelerTargetDb = std::clamp(parameters.loudnessTarget.load(std::memory_order_relaxed),
                                            -30.0f, -8.0f);
    const auto failsafeForced = parameters.forceFailsafe.load(std::memory_order_relaxed);
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

    const auto compressorThreshold = -5.0f - dynamics * 15.0f
        + adaptiveTargets.compressionDb.load(std::memory_order_relaxed);
    const auto compressorRatio = 1.10f + dynamics * 1.65f;
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
    const auto limiterRelease = std::exp(-1.0f / static_cast<float>(sampleRate * 0.080));
    // The leveler gain still ramps, but only enough to stop zipper noise: the
    // Kalman estimate it follows is already smooth by construction.
    const auto levelGainSmoothing = std::exp(-1.0f / static_cast<float>(sampleRate * 0.020));

    auto blockMaxCompressorReduction = 0.0f;
    auto blockMaxMakeup = 1.0f;
    auto blockMaxDeEsser = 0.0f;
    auto blockMaxDynamicEq = 0.0f;
    auto blockMaxLimiterReduction = 0.0f;
    auto blockTruePeak = 0.0f;
    const auto processedSelected = !parameters.bypass.load(std::memory_order_relaxed)
        && parameters.abProcessed.load(std::memory_order_relaxed);
    wetMix.setTargetValue(processedSelected ? 1.0f : 0.0f);

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

        for (int channel = 0; channel < activeChannels; ++channel)
        {
            auto value = toned[channel];
            for (int band = 0; band < dynamicBandCount; ++band)
                value += (dynamicBands[static_cast<size_t>(band)].getGain() - 1.0f)
                    * dynamicSignal[band][channel];
            float lowGroup = 0.0f, highGroup = 0.0f;
            middleSplit.processSample(channel, value, lowGroup, highGroup);
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

        const auto blockOutputGain = outputGain.getNextValue();
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
            const auto value = lowGroupPhase.processSample(channel, compressedLowGroup)
                + highGroupPhase.processSample(channel, compressedMid + compressedTop);
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
        // TEMP-NEGATIVE-CHECK
        for (int channel = 0; channel < maximumChannels; ++channel)
        {
            auto* samples = preSaturationBuffer.getWritePointer(channel);
            for (int i = 0; i < numSamples; ++i)
                samples[i] = std::tanh(samples[i] * saturationDrive) * saturationNormaliser;
        }
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
        if (activeChannels == 2 && (monoCompatibility || safeWidth < 0.9999f))
        {
            const auto mid = 0.5f * (wet[0] + wet[1]);
            auto side = 0.5f * (wet[0] - wet[1]);
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

        // Summed across channels, not averaged, because that is what BS.1770
        // does and it is what makes the reading comparable to the LUFS number
        // the operator set as a target. The detector is a one-pole rather than
        // 400 ms blocks, so this is an approximation of loudness and not a
        // conformant measurement, but it is weighted like one: a flat RMS on
        // worship is dominated by the kick and the bass, and the same voice
        // ends up levelled differently depending on what is playing under it.
        auto programmePower = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto weighted = levelerWeighting.process(channel, wet[channel]);
            programmePower += weighted * weighted;
        }
        programmeLevelSquare = levelDetectorCoefficient * programmeLevelSquare
            + (1.0f - levelDetectorCoefficient) * programmePower;
        const auto measuredLevelDb = programmeLevelSquare > 1.0e-12f
            ? gainToDecibels(std::sqrt(programmeLevelSquare)) + KWeighting::lufsOffsetDb
            : -100.0f;

        gateLevelDb += gateLevelCoefficient * (measuredLevelDb - gateLevelDb);
        sectionRebaseSamples = std::max(0.0f, sectionRebaseSamples - 1.0f);

        // Tracked whether the gate is open or not: deciding what a closed gate
        // is looking at is exactly what this is for.
        programmeLevelTrend += levelTrendCoefficient * (measuredLevelDb - programmeLevelTrend);
        levelVariationDb += levelTrendCoefficient
            * (std::abs(measuredLevelDb - programmeLevelTrend) - levelVariationDb);

        // Gate first. A closed gate freezes both the estimate and the gain, so
        // a pause cannot drag the programme level down and then be levelled
        // back up as room noise when the speaker stops.
        if (gateLevelDb > levelerAbsoluteGateDb)
        {
            silenceSamples = 0.0f;
            loudnessSampleCount += 1.0f;
            // Cumulative mean while the window is still filling, exponential
            // afterwards. BS.1770 averages every gated block for the same
            // reason: the relative gate is meaningless until the reference it
            // is relative to actually reflects the programme.
            const auto alpha = std::max(1.0f / loudnessSampleCount, 1.0f - loudnessAverageCoefficient);
            programmeLoudnessAverage += alpha * (gateLevelDb - programmeLoudnessAverage);
            // Both sides of the comparison are integrated. Syllables swing about
            // 12 dB peak to valley, wider than the 10 dB relative gate, so
            // gating on the fast detector makes every loud phrase close the gate
            // on its own quiet half. BS.1770 gates on 400 ms blocks for exactly
            // this reason.
            const auto relativeThreshold = programmeLoudnessAverage - levelerRelativeGateDb
                + (levelerGateOpen ? 0.0f : levelerGateHysteresisDb);
            levelerGateOpen = gateLevelDb > relativeThreshold;
        }
        else
        {
            silenceSamples += 1.0f;
            levelerGateOpen = false;
        }

        if (levelerGateOpen)
        {
            gateClosedSamples = 0.0f;
        }
        else if (silenceSamples <= 0.0f)
        {
            // Only counted while there is still programme present. Real silence
            // is handled by the release path below, not by rebasing onto it.
            if (gateClosedSamples <= 0.0f)
            {
                // The measurement window starts here. The step that closed the
                // gate is itself a large deviation, and letting it into the
                // variation would make every pause look like speech for the
                // first few seconds -- which is precisely the span being judged.
                programmeLevelTrend = measuredLevelDb;
                levelVariationDb = 0.0f;
            }
            gateClosedSamples += 1.0f;
            if (gateClosedSamples > gateHoldSamples && levelVariationDb > levelerSpeechVariationDb)
            {
                programmeLoudnessAverage = gateLevelDb;
                loudnessSampleCount = 1.0f;
                levelerGateOpen = true;
                gateClosedSamples = 0.0f;
                // The gate has just concluded this is a new section. Making the
                // filter rediscover that from its own innovation would waste the
                // second it takes to build up, and the conclusion is already in
                // hand: hand it over.
                programmeInnovationAverage = kalmanInnovationCeilingDb;
                sectionRebaseSamples = sectionRebaseLength;
            }
        }

        if (levelerGateOpen)
        {
            if (!programmeLevelInitialised)
            {
                programmeLevelEstimateDb = measuredLevelDb;
                programmeLevelInitialised = true;
            }

            const auto innovation = measuredLevelDb - programmeLevelEstimateDb;
            programmeInnovationAverage = innovationCoefficient * programmeInnovationAverage
                + (1.0f - innovationCoefficient) * std::abs(innovation);
            // Sustained innovation means the programme really moved, not that a
            // syllable was loud. Only then is it worth abandoning the slow
            // estimate, which is the whole anti-pumping argument.
            // Rising programme is unambiguous -- the mix got louder and the
            // gain has to come down now -- so it may always accelerate. Falling
            // programme may not, unless the gate has already accepted it as a
            // section rather than a pause.
            const auto mayAccelerate = innovation > 0.0f || sectionRebaseSamples > 0.0f;
            const auto sectionChange = mayAccelerate
                ? std::clamp((programmeInnovationAverage - kalmanInnovationFloorDb)
                                 / (kalmanInnovationCeilingDb - kalmanInnovationFloorDb), 0.0f, 1.0f)
                : 0.0f;
            const auto steadyStateGain = kalmanSteadyGain
                + sectionChange * (kalmanFastGain - kalmanSteadyGain);
            const auto processNoise = steadyStateGain * steadyStateGain * kalmanMeasurementNoise;

            const auto predictedCovariance = programmeLevelCovariance + processNoise;
            const auto kalmanGain = predictedCovariance / (predictedCovariance + kalmanMeasurementNoise);
            programmeLevelEstimateDb += kalmanGain * innovation;
            programmeLevelCovariance = (1.0f - kalmanGain) * predictedCovariance;
        }
        else if (programmeLevelInitialised && silenceSamples > silenceReleaseSamples)
        {
            // Standing down is expressed as the estimate drifting to the
            // target, not as an override on the gain: one state variable stays
            // in charge, so when programme returns the gain is already
            // continuous and the Kalman simply picks up from where it is.
            programmeLevelEstimateDb += silenceReleaseGain
                * (levelerTargetDb - programmeLevelEstimateDb);
            // Nothing has been measured for over a second, so the estimate is
            // worth very little. Saying so is what lets it re-lock quickly.
            programmeLevelCovariance = kalmanInitialCovariance;
            programmeInnovationAverage = 0.0f;
        }

        // Recovery is capped so this cannot turn room noise into a programme
        // even if the gate is fooled.
        auto levelTargetGain = programmeLevelGain;
        if (levelerIsEnabled && processedSelected && programmeLevelInitialised
            && (levelerGateOpen || silenceSamples > silenceReleaseSamples))
            levelTargetGain = decibelsToGain(std::clamp(levelerTargetDb - programmeLevelEstimateDb,
                                                        levelerMaximumCutDb, levelerMaximumBoostDb));
        else if (!levelerIsEnabled || !processedSelected)
            levelTargetGain = 1.0f;
        programmeLevelGain = levelGainSmoothing * programmeLevelGain
            + (1.0f - levelGainSmoothing) * levelTargetGain;
        for (int channel = 0; channel < activeChannels; ++channel)
            wet[channel] *= programmeLevelGain;

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

    lastCompressorReduction = std::max(blockMaxCompressorReduction, lastCompressorReduction * 0.92f);
    lastLimiterReduction = std::max(blockMaxLimiterReduction, lastLimiterReduction * 0.90f);
    metrics.compressorGainReductionDb.store(lastCompressorReduction, std::memory_order_release);
    metrics.limiterGainReductionDb.store(lastLimiterReduction, std::memory_order_release);
    metrics.truePeakEstimate.store(blockTruePeak, std::memory_order_release);
    metrics.appliedOutputGainDb.store(gainToDecibels(outputGain.getCurrentValue()), std::memory_order_release);
    metrics.deEsserReductionDb.store(blockMaxDeEsser, std::memory_order_release);
    metrics.dynamicEqReductionDb.store(blockMaxDynamicEq, std::memory_order_release);
    metrics.compressorMakeupDb.store(gainToDecibels(blockMaxMakeup, 0.0f), std::memory_order_release);
    metrics.broadcastLevelGainDb.store(gainToDecibels(programmeLevelGain), std::memory_order_release);
    metrics.abMatchGainDb.store(gainToDecibels(dryMatchGain.getCurrentValue(), -24.0f), std::memory_order_release);
    metrics.failsafeActive.store(failsafeBlend > 0.0f, std::memory_order_release);
    metrics.failsafeEngagements.store(failsafeEngagementCount, std::memory_order_release);
    metrics.nonFiniteInputSamples.store(nonFiniteInputCount, std::memory_order_release);
    metrics.programmeCorrelation.store(measuredCorrelation, std::memory_order_release);
    metrics.appliedStereoWidth.store(std::min(stereoWidth.getCurrentValue(), coherenceWidth.getCurrentValue()),
                                     std::memory_order_release);
    metrics.programmeLevelDb.store(programmeLevelEstimateDb, std::memory_order_release);
    metrics.levelerGateOpen.store(levelerGateOpen, std::memory_order_release);
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
                                         + (smart ? adaptiveTargets.mudGainDb.load(std::memory_order_relaxed) : 0.0f),
                                     autoMode ? -3.0f : -2.0f, 0.0f));
    clarityGain.setTargetValue(std::clamp(clarity * 1.8f
                                             + (smart ? adaptiveTargets.clarityGainDb.load(std::memory_order_relaxed) : 0.0f),
                                         -0.5f, autoMode ? 3.5f : 3.0f));
    harshGain.setTargetValue(smart ? std::clamp(adaptiveTargets.harshGainDb.load(std::memory_order_relaxed), autoMode ? -3.0f : -2.0f, 0.0f) : 0.0f);
    sibilanceGain.setTargetValue(smart ? std::clamp(adaptiveTargets.sibilanceGainDb.load(std::memory_order_relaxed), autoMode ? -4.0f : -2.5f, 0.0f) : 0.0f);
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
