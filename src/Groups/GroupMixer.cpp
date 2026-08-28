#include "GroupMixer.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
// Same five zones the analyser and the Smart Engine already use.
constexpr std::array<float, 5> bandCentres { 60.0f, 240.0f, 1100.0f, 4500.0f, 12000.0f };
constexpr std::array<float, 5> bandQ { 0.70f, 0.75f, 0.62f, 0.62f, 0.70f };
// The masking controller only ever moves bands 2 and 3.
constexpr float presenceHz = 1100.0f;
constexpr float upperHz = 4500.0f;
constexpr unsigned char voiceActiveFlag = 1U << 0U;
constexpr unsigned char activeFlag = 1U << 1U;
constexpr unsigned char appliedFlag = 1U << 2U;
}

GroupMixer::GroupMixer() noexcept
{
    publishDecision({});
}

void GroupMixer::BandAnalyser::configure(double rate) noexcept
{
    for (size_t band = 0; band < filters.size(); ++band)
        filters[band].setBandPass(rate, bandCentres[band], bandQ[band]);
    reset();
}

void GroupMixer::BandAnalyser::reset() noexcept
{
    for (auto& filter : filters)
        filter.reset();
    energy.fill(0.0);
    totalSquares = 0.0;
}

void GroupMixer::BandAnalyser::push(float sample) noexcept
{
    totalSquares += static_cast<double>(sample) * sample;
    for (size_t band = 0; band < filters.size(); ++band)
    {
        const auto filtered = filters[band].process(0, sample);
        energy[band] += static_cast<double>(filtered) * filtered;
    }
}

GroupFeatures GroupMixer::BandAnalyser::finish(int sampleCount) noexcept
{
    GroupFeatures features;
    if (sampleCount <= 0) return features;

    double total = 0.0;
    for (const auto value : energy)
        total += value;
    for (size_t band = 0; band < energy.size(); ++band)
        features.bandEnergy[band] = total > 1.0e-18
            ? static_cast<float>(energy[band] / total) : 0.0f;

    const auto rms = std::sqrt(totalSquares / sampleCount);
    features.rmsDb = rms > 1.0e-6 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;

    // Speech concentrates in the two intelligibility bands and is not loud in
    // the extremes. This is a cheap likelihood, not a classifier.
    const auto intelligibility = features.bandEnergy[2] + features.bandEnergy[3];
    const auto extremes = features.bandEnergy[0] + features.bandEnergy[4];
    features.voiceProbability = std::clamp((intelligibility - 0.45f) / 0.30f, 0.0f, 1.0f)
        * std::clamp(1.0f - extremes * 1.8f, 0.0f, 1.0f)
        * (features.rmsDb > -45.0f ? 1.0f : 0.0f);

    energy.fill(0.0);
    totalSquares = 0.0;
    return features;
}

void GroupMixer::prepare(double newSampleRate) noexcept
{
    sampleRate = std::max(8000.0, newSampleRate);
    voiceAnalyser.configure(sampleRate);
    musicAnalyser.configure(sampleRate);
    presenceGain.reset(sampleRate, 0.25);
    upperGain.reset(sampleRate, 0.25);
    presenceGain.setCurrentAndTargetValue(0.0f);
    upperGain.setCurrentAndTargetValue(0.0f);
    reset();
}

void GroupMixer::reset() noexcept
{
    voiceAnalyser.reset();
    musicAnalyser.reset();
    masking.reset();
    for (auto& filter : musicMaskFilters)
        filter.reset();
    presenceGain.setCurrentAndTargetValue(0.0f);
    upperGain.setCurrentAndTargetValue(0.0f);
    decision = {};
    publishDecision(decision);
}

bool GroupMixer::process(const float* const* inputs, int inputCount,
                         float* const* outputs, int outputCount, int sampleCount,
                         const GroupRoutingConfig& routes) noexcept
{
    if (inputs == nullptr || outputs == nullptr || sampleCount <= 0 || outputCount < 2)
        return false;
    if (!routes.voice.validFor(inputCount) || !routes.music.validFor(inputCount)
        || !routes.ambience.validFor(inputCount))
        return false;

    const auto* voiceLeft = inputs[routes.voice.leftChannel];
    const auto* voiceRight = inputs[routes.voice.rightChannel];
    const auto* musicLeft = inputs[routes.music.leftChannel];
    const auto* musicRight = inputs[routes.music.rightChannel];
    const auto* ambienceLeft = inputs[routes.ambience.leftChannel];
    const auto* ambienceRight = inputs[routes.ambience.rightChannel];
    if (voiceLeft == nullptr || voiceRight == nullptr || musicLeft == nullptr
        || musicRight == nullptr || ambienceLeft == nullptr || ambienceRight == nullptr)
        return false;

    const auto maskingOn = maskingEnabled.load(std::memory_order_acquire);
    const auto current = decision;
    presenceGain.setTargetValue(maskingOn ? current.musicGainDb[2] : 0.0f);
    upperGain.setTargetValue(maskingOn ? current.musicGainDb[3] : 0.0f);
    // Coefficients are recomputed once per block, not per sample: the masking
    // gains ramp over 250 ms, so a block of resolution is more than enough and
    // trigonometry has no business inside the sample loop.
    if (maskingOn)
    {
        musicMaskFilters[0].setPeak(sampleRate, presenceHz, 0.90f, presenceGain.getCurrentValue());
        musicMaskFilters[1].setPeak(sampleRate, upperHz, 0.90f, upperGain.getCurrentValue());
    }

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto voiceMono = 0.5f * (voiceLeft[sample] + voiceRight[sample]);
        const auto musicMonoRaw = 0.5f * (musicLeft[sample] + musicRight[sample]);
        voiceAnalyser.push(voiceMono);
        musicAnalyser.push(musicMonoRaw);

        auto left = musicLeft[sample];
        auto right = musicRight[sample];
        if (maskingOn)
        {
            left = musicMaskFilters[0].process(0, left);
            left = musicMaskFilters[1].process(0, left);
            right = musicMaskFilters[0].process(1, right);
            right = musicMaskFilters[1].process(1, right);
        }

        // The ambience stem stays lower: voice and music keep their console
        // balance while the room microphones only add space.
        outputs[0][sample] = voiceLeft[sample] + left + 0.35f * ambienceLeft[sample];
        outputs[1][sample] = voiceRight[sample] + right + 0.35f * ambienceRight[sample];
    }

    presenceGain.skip(sampleCount);
    upperGain.skip(sampleCount);

    for (int channel = 2; channel < outputCount; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear(outputs[channel], sampleCount);

    const auto voiceFeatures = voiceAnalyser.finish(sampleCount);
    const auto musicFeatures = musicAnalyser.finish(sampleCount);
    const auto elapsed = static_cast<float>(sampleCount / sampleRate);
    auto updated = masking.update(voiceFeatures, musicFeatures, elapsed);
    updated.applied = maskingOn && updated.active;
    decision = updated;
    publishDecision(decision);
    return true;
}

void GroupMixer::publishDecision(const MaskingDecision& value) noexcept
{
    publishedVersion.fetch_add(1U, std::memory_order_release);
    for (size_t band = 0; band < publishedMusicGainDb.size(); ++band)
        publishedMusicGainDb[band].store(value.musicGainDb[band], std::memory_order_relaxed);
    publishedConfidence.store(value.confidence, std::memory_order_relaxed);

    auto flags = static_cast<unsigned char>(0U);
    if (value.voiceActive) flags |= voiceActiveFlag;
    if (value.active) flags |= activeFlag;
    if (value.applied) flags |= appliedFlag;
    publishedFlags.store(flags, std::memory_order_relaxed);
    publishedVersion.fetch_add(1U, std::memory_order_release);
}

MaskingDecision GroupMixer::getDecision() const noexcept
{
    MaskingDecision snapshot;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = publishedVersion.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) continue;

        for (size_t band = 0; band < snapshot.musicGainDb.size(); ++band)
            snapshot.musicGainDb[band] = publishedMusicGainDb[band].load(std::memory_order_relaxed);
        snapshot.confidence = publishedConfidence.load(std::memory_order_relaxed);
        const auto flags = publishedFlags.load(std::memory_order_relaxed);
        snapshot.voiceActive = (flags & voiceActiveFlag) != 0U;
        snapshot.active = (flags & activeFlag) != 0U;
        snapshot.applied = (flags & appliedFlag) != 0U;

        if (before == publishedVersion.load(std::memory_order_acquire))
            return snapshot;
    }
    return snapshot;
}
} // namespace churchstream
