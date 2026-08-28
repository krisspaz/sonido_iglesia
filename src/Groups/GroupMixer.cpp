#include "GroupMixer.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
namespace psy = psychoacoustics;
// The bands the intelligibility model is defined on. Q comes from the width of
// each critical band rather than a constant: a critical band is about 100 Hz
// wide at the bottom of the range and a fifth of an octave at the top, and a
// fixed Q would measure the wrong thing at one end or the other.
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
    {
        const auto centre = psy::criticalBandCentresHz[band];
        const auto bark = psy::barkFromHertz(centre);
        const auto width = std::max(50.0f, psy::hertzFromBark(bark + 0.5f)
                                         - psy::hertzFromBark(bark - 0.5f));
        filters[band].setBandPass(rate, centre, centre / width);
    }
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
    {
        const auto bandRms = std::sqrt(energy[band] / sampleCount);
        features.bandLevelDb[band] = bandRms > 1.0e-7
            ? static_cast<float>(20.0 * std::log10(bandRms)) : -100.0f;
    }

    const auto rms = std::sqrt(totalSquares / sampleCount);
    features.rmsDb = rms > 1.0e-6 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;

    // Speech concentrates where the SII says it carries information and is not
    // loud at the extremes. This is a cheap likelihood, not a classifier, but
    // weighting it by the importance function costs nothing and is at least
    // measuring the right thing.
    auto weighted = 0.0;
    for (size_t band = 0; band < energy.size(); ++band)
        weighted += energy[band] * static_cast<double>(psy::speechImportance[band]);
    const auto speechShare = total > 1.0e-18 ? static_cast<float>(weighted / total) : 0.0f;
    features.voiceProbability = std::clamp((speechShare - 0.030f) / 0.025f, 0.0f, 1.0f)
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
    for (auto& gain : zoneGain)
    {
        gain.reset(sampleRate, 0.25);
        gain.setCurrentAndTargetValue(0.0f);
    }
    reset();
}

void GroupMixer::reset() noexcept
{
    voiceAnalyser.reset();
    musicAnalyser.reset();
    masking.reset();
    for (auto& filter : musicMaskFilters)
        filter.reset();
    for (auto& gain : zoneGain)
        gain.setCurrentAndTargetValue(0.0f);
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
    for (int zone = 0; zone < maskingZoneCount; ++zone)
        zoneGain[static_cast<size_t>(zone)].setTargetValue(
            maskingOn ? current.musicGainDb[static_cast<size_t>(zone)] : 0.0f);
    // Coefficients are recomputed once per block, not per sample: the masking
    // gains ramp over 250 ms, so a block of resolution is more than enough and
    // trigonometry has no business inside the sample loop.
    if (maskingOn)
        for (int zone = 0; zone < maskingZoneCount; ++zone)
            musicMaskFilters[static_cast<size_t>(zone)].setPeak(
                sampleRate, maskingZoneCentresHz[static_cast<size_t>(zone)], 0.90f,
                zoneGain[static_cast<size_t>(zone)].getCurrentValue());

    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto voiceMono = 0.5f * (voiceLeft[sample] + voiceRight[sample]);
        const auto musicMonoRaw = 0.5f * (musicLeft[sample] + musicRight[sample]);
        voiceAnalyser.push(voiceMono);
        musicAnalyser.push(musicMonoRaw);

        auto left = musicLeft[sample];
        auto right = musicRight[sample];
        if (maskingOn)
            for (auto& filter : musicMaskFilters)
            {
                left = filter.process(0, left);
                right = filter.process(1, right);
            }

        // The ambience stem stays lower: voice and music keep their console
        // balance while the room microphones only add space.
        outputs[0][sample] = voiceLeft[sample] + left + 0.35f * ambienceLeft[sample];
        outputs[1][sample] = voiceRight[sample] + right + 0.35f * ambienceRight[sample];
    }

    for (auto& gain : zoneGain)
        gain.skip(sampleCount);

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
    publishedIntelligibility.store(value.speechIntelligibility, std::memory_order_relaxed);

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
        snapshot.speechIntelligibility = publishedIntelligibility.load(std::memory_order_relaxed);
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
