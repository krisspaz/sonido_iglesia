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

float decibelsToGain(float decibels) noexcept
{
    return std::pow(10.0f, decibels / 20.0f);
}

float gainToDecibels(float gain, float floor = -100.0f) noexcept
{
    return gain > 0.0f ? std::max(floor, 20.0f * std::log10(gain)) : floor;
}
}

void ProcessingEngine::prepare(double newSampleRate, int maximumBlockSize, int channels)
{
    sampleRate = std::max(8000.0, newSampleRate);
    preparedBlockSize = std::max(1, maximumBlockSize);
    preparedChannels = std::clamp(channels, 1, maximumChannels);
    // The lookahead must stay longer than the true-peak detector latency,
    // otherwise the limiter would react after the peak it is protecting from.
    lookaheadSamples = std::clamp(static_cast<int>(std::ceil(sampleRate * 0.001)),
                                  TruePeakDetector::latencySamples * 2,
                                  maximumLookaheadSamples - 1);
    dryBuffer.setSize(preparedChannels, preparedBlockSize, false, true, false);

    const juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(preparedBlockSize),
                                         static_cast<juce::uint32>(preparedChannels) };
    for (auto* filter : { &middleSplit, &lowSplit, &highSplit, &lowGroupPhase, &highGroupPhase })
        filter->prepare(spec);
    middleSplit.setCutoffFrequency(500.0f);
    lowSplit.setCutoffFrequency(120.0f);
    highSplit.setCutoffFrequency(4000.0f);
    lowGroupPhase.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    lowGroupPhase.setCutoffFrequency(4000.0f);
    highGroupPhase.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
    highGroupPhase.setCutoffFrequency(120.0f);

    for (auto* value : { &rumbleCutoff, &warmthGain, &lowGain, &mudGain, &clarityGain,
                         &harshGain, &sibilanceGain, &highGain, &outputGain })
        value->reset(sampleRate, 0.75);
    wetMix.reset(sampleRate, 0.05);
    dryMatchGain.reset(sampleRate, 0.25);
    // ~1.5 s integration: slow enough to track programme level instead of
    // individual syllables, fast enough to settle before the operator judges.
    loudnessMatchCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * 1.5));
    stereoWidth.reset(sampleRate, 5.0);
    stereoBalance.reset(sampleRate, 5.0);

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
    dryMatchGain.setCurrentAndTargetValue(1.0f);
    reset();
    configureFilters();
}

void ProcessingEngine::reset() noexcept
{
    for (auto& channel : wetDelay)
        channel.fill(0.0f);
    for (auto& channel : dryDelay)
        channel.fill(0.0f);
    truePeakDetector.reset();
    dcPreviousInput.fill(0.0f);
    dcPreviousOutput.fill(0.0f);
    rumbleFilter.reset();
    warmthFilter.reset();
    lowFilter.reset();
    mudFilter.reset();
    clarityFilter.reset();
    harshFilter.reset();
    sibilanceFilter.reset();
    highFilter.reset();
    middleSplit.reset();
    lowSplit.reset();
    highSplit.reset();
    lowGroupPhase.reset();
    highGroupPhase.reset();
    bandEnvelope.fill(0.0f);
    bandGain.fill(1.0f);
    limiterGain = 1.0f;
    lastCompressorReduction = 0.0f;
    lastLimiterReduction = 0.0f;
    delayWritePosition = 0;
    metrics.compressorGainReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.limiterGainReductionDb.store(0.0f, std::memory_order_relaxed);
    metrics.truePeakEstimate.store(0.0f, std::memory_order_relaxed);
    metrics.abMatchGainDb.store(0.0f, std::memory_order_relaxed);
    dryLoudnessSquare = 0.0;
    wetLoudnessSquare = 0.0;
    dryMatchGain.setCurrentAndTargetValue(1.0f);
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
    const auto limiterIsEnabled = parameters.limiterEnabled.load(std::memory_order_relaxed);

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

    auto blockMaxCompressorReduction = 0.0f;
    auto blockMaxLimiterReduction = 0.0f;
    auto blockTruePeak = 0.0f;
    const auto processedSelected = !parameters.bypass.load(std::memory_order_relaxed)
        && parameters.abProcessed.load(std::memory_order_relaxed);
    wetMix.setTargetValue(processedSelected ? 1.0f : 0.0f);

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        float wet[maximumChannels] { 0.0f, 0.0f };
        float original[maximumChannels] { 0.0f, 0.0f };
        float bands[maximumChannels][4] {};

        for (int channel = 0; channel < activeChannels; ++channel)
        {
            original[channel] = channels[channel][sampleIndex];
            auto value = processDcBlocker(channel, original[channel]);
            if (rumbleIsEnabled)
                value = rumbleFilter.process(channel, value);
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
            float lowGroup = 0.0f, highGroup = 0.0f;
            middleSplit.processSample(channel, value, lowGroup, highGroup);
            lowSplit.processSample(channel, lowGroup, bands[channel][0], bands[channel][1]);
            highSplit.processSample(channel, highGroup, bands[channel][2], bands[channel][3]);
        }

        for (size_t band = 0; band < bandGain.size(); ++band)
        {
            auto detector = std::abs(bands[0][band]);
            if (activeChannels > 1) detector = std::max(detector, std::abs(bands[1][band]));
            const auto envelopeCoefficient = detector > bandEnvelope[band]
                ? envelopeAttack[band] : envelopeRelease[band];
            bandEnvelope[band] = envelopeCoefficient * bandEnvelope[band]
                + (1.0f - envelopeCoefficient) * detector;
            auto targetGain = 1.0f;
            if (compressorIsEnabled)
            {
                const auto inputDb = gainToDecibels(bandEnvelope[band]);
                const auto threshold = compressorThreshold + bandThresholdOffset[band];
                if (inputDb > threshold)
                {
                    const auto outputDb = threshold + (inputDb - threshold) / compressorRatio;
                    targetGain = decibelsToGain(outputDb - inputDb);
                }
            }
            const auto coefficient = targetGain < bandGain[band] ? gainAttack : gainRelease;
            bandGain[band] = coefficient * bandGain[band] + (1.0f - coefficient) * targetGain;
            blockMaxCompressorReduction = std::max(blockMaxCompressorReduction,
                                                    std::max(0.0f, -gainToDecibels(bandGain[band], -60.0f)));
        }

        const auto blockOutputGain = outputGain.getNextValue();
        const auto width = stereoWidth.getNextValue();
        const auto balance = stereoBalance.getNextValue();
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto compressedLowGroup = bands[channel][0] * bandGain[0] + bands[channel][1] * bandGain[1];
            const auto compressedHighGroup = bands[channel][2] * bandGain[2] + bands[channel][3] * bandGain[3];
            auto value = lowGroupPhase.processSample(channel, compressedLowGroup)
                + highGroupPhase.processSample(channel, compressedHighGroup);
            if (saturationIsEnabled)
                value = std::tanh(value * saturationDrive) * saturationNormaliser;
            wet[channel] = value * blockOutputGain;
        }

        if (activeChannels == 2 && width < 0.9999f)
        {
            const auto mid = 0.5f * (wet[0] + wet[1]);
            const auto side = 0.5f * (wet[0] - wet[1]) * width;
            wet[0] = mid + side;
            wet[1] = mid - side;
        }
        if (activeChannels == 2 && std::abs(balance) > 1.0e-5f)
        {
            wet[0] *= decibelsToGain(balance * 0.5f);
            wet[1] *= decibelsToGain(-balance * 0.5f);
        }

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
        const auto limiterReduction = std::max(0.0f, -gainToDecibels(limiterGain, -60.0f));
        blockMaxLimiterReduction = std::max(blockMaxLimiterReduction, limiterReduction);

        const auto mix = wetMix.getNextValue();
        const auto matchGain = dryMatchGain.getNextValue();

        auto dryMono = 0.0f;
        auto wetMono = 0.0f;
        for (int channel = 0; channel < activeChannels; ++channel)
        {
            const auto delayedWet = wetDelay[static_cast<size_t>(channel)][static_cast<size_t>(delayWritePosition)];
            const auto delayedDry = dryDelay[static_cast<size_t>(channel)][static_cast<size_t>(delayWritePosition)];
            wetDelay[static_cast<size_t>(channel)][static_cast<size_t>(delayWritePosition)] = wet[channel];
            dryDelay[static_cast<size_t>(channel)][static_cast<size_t>(delayWritePosition)] = original[channel];
            const auto limitedWet = delayedWet * limiterGain;
            const auto matchedDry = delayedDry * matchGain;
            dryMono += delayedDry;
            wetMono += limitedWet;
            channels[channel][sampleIndex] = matchedDry + (limitedWet - matchedDry) * mix;
        }
        const auto monoScale = activeChannels > 1 ? 0.5f : 1.0f;
        updateLoudnessMatch(dryMono * monoScale, wetMono * monoScale);

        if (++delayWritePosition >= lookaheadSamples)
            delayWritePosition = 0;
    }

    lastCompressorReduction = std::max(blockMaxCompressorReduction, lastCompressorReduction * 0.92f);
    lastLimiterReduction = std::max(blockMaxLimiterReduction, lastLimiterReduction * 0.90f);
    metrics.compressorGainReductionDb.store(lastCompressorReduction, std::memory_order_release);
    metrics.limiterGainReductionDb.store(lastLimiterReduction, std::memory_order_release);
    metrics.truePeakEstimate.store(blockTruePeak, std::memory_order_release);
    metrics.appliedOutputGainDb.store(gainToDecibels(outputGain.getCurrentValue()), std::memory_order_release);
    metrics.abMatchGainDb.store(gainToDecibels(dryMatchGain.getCurrentValue(), -24.0f), std::memory_order_release);
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
    const auto output = sample - dcPreviousInput[index] + 0.995f * dcPreviousOutput[index];
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

    const auto adaptiveRumble = smart ? adaptiveTargets.rumbleCutoffHz.load(std::memory_order_relaxed) : 20.0f;
    rumbleCutoff.setTargetValue(std::clamp(std::max(20.0f + clean * 15.0f, adaptiveRumble), 20.0f, 55.0f));
    warmthGain.setTargetValue(std::clamp(warmth * 0.8f, 0.0f, 0.8f));
    lowGain.setTargetValue(smart ? std::clamp(adaptiveTargets.lowGainDb.load(std::memory_order_relaxed), autoMode ? -2.0f : -1.5f, 0.5f) : 0.0f);
    mudGain.setTargetValue(std::clamp(-clean * 0.8f
                                         + (smart ? adaptiveTargets.mudGainDb.load(std::memory_order_relaxed) : 0.0f),
                                     autoMode ? -3.0f : -2.0f, 0.0f));
    clarityGain.setTargetValue(std::clamp(clarity * 1.1f
                                             + (smart ? adaptiveTargets.clarityGainDb.load(std::memory_order_relaxed) : 0.0f),
                                         -0.5f, autoMode ? 2.5f : 2.0f));
    harshGain.setTargetValue(smart ? std::clamp(adaptiveTargets.harshGainDb.load(std::memory_order_relaxed), autoMode ? -3.0f : -2.0f, 0.0f) : 0.0f);
    sibilanceGain.setTargetValue(smart ? std::clamp(adaptiveTargets.sibilanceGainDb.load(std::memory_order_relaxed), autoMode ? -4.0f : -2.5f, 0.0f) : 0.0f);
    highGain.setTargetValue(smart ? std::clamp(adaptiveTargets.highGainDb.load(std::memory_order_relaxed), autoMode ? -2.0f : -1.5f, 0.5f) : 0.0f);
    const auto targetOutputDb = smart
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
        matchTarget = std::clamp(static_cast<float>(std::sqrt(wetLoudnessSquare / dryLoudnessSquare)),
                                 decibelsToGain(-12.0f), decibelsToGain(12.0f));
    dryMatchGain.setTargetValue(matchTarget);
    dryMatchGain.skip(numSamples);
}

void ProcessingEngine::configureFilters() noexcept
{
    rumbleFilter.setHighPass(sampleRate, rumbleCutoff.getCurrentValue(), 0.70710678f);
    warmthFilter.setPeak(sampleRate, 110.0f, 0.75f, warmthGain.getCurrentValue());
    lowFilter.setPeak(sampleRate, 90.0f, 0.85f, lowGain.getCurrentValue());
    mudFilter.setPeak(sampleRate, 240.0f, 0.80f, mudGain.getCurrentValue());
    clarityFilter.setPeak(sampleRate, 2500.0f, 0.75f, clarityGain.getCurrentValue());
    harshFilter.setPeak(sampleRate, 5200.0f, 0.90f, harshGain.getCurrentValue());
    // Narrow enough to sit under sibilance without dulling the whole top end.
    sibilanceFilter.setPeak(sampleRate, 7400.0f, 3.20f, sibilanceGain.getCurrentValue());
    highFilter.setPeak(sampleRate, 12000.0f, 0.70f, highGain.getCurrentValue());
}

float ProcessingEngine::clampControl(float value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}
} // namespace churchstream
