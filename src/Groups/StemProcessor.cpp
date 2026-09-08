#include "Groups/StemProcessor.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
// The de-esser only cuts the sibilance band, not the whole stem. The scale is
// how far (dB) above the threshold a sibilant must sit before its band is fully
// ducked.
constexpr float deEsserThresholdDb = -32.0f;
constexpr float deEsserRangeDb = 6.0f;
constexpr float deEsserDepth = 0.55f;
} // namespace

void StemProcessor::setOnePole(double sampleRate, float seconds, float& coeff) noexcept
{
    const auto tau = static_cast<double>(std::max(seconds, 0.0005f));
    coeff = static_cast<float>(1.0 - std::exp(-1.0 / (sampleRate * tau)));
}

void StemProcessor::setExponentialRelease(double sampleRate, float seconds, float& coeff) noexcept
{
    coeff = static_cast<float>(std::exp(-1.0 / (sampleRate * static_cast<double>(std::max(seconds, 0.001f)))));
}

void StemProcessor::prepare(double newSampleRate) noexcept
{
    const auto sampleRate = std::max(8000.0, newSampleRate);

    voice.highPass.setHighPass(sampleRate, 80.0f);
    voice.deEsserBand.setBandPass(sampleRate, 6500.0f, 1.5f);
    voice.presence.setHighShelf(sampleRate, 4500.0f, 0.70710678f, 3.0f);
    voice.deEsserLevel = 0.0f;
    setExponentialRelease(sampleRate, 0.090f, voice.deEsserReleaseCoeff);

    voice.compressor.thresholdDb = -26.0f;
    voice.compressor.ratio = 3.0f;
    voice.compressor.attackSeconds = 0.012f;
    voice.compressor.releaseSeconds = 0.150f;
    voice.compressor.makeupDb = 1.5f;
    voice.compressor.detectorDb = -96.0f;
    voice.compressor.gainDb = 0.0f;
    setOnePole(sampleRate, voice.compressor.attackSeconds, voice.compressor.attackCoeff);
    setOnePole(sampleRate, voice.compressor.releaseSeconds, voice.compressor.releaseCoeff);

    music.highPass.setHighPass(sampleRate, 40.0f);
    music.compressor.thresholdDb = -22.0f;
    music.compressor.ratio = 2.5f;
    music.compressor.attackSeconds = 0.008f;
    music.compressor.releaseSeconds = 0.120f;
    music.compressor.makeupDb = 0.0f;
    music.compressor.detectorDb = -96.0f;
    music.compressor.gainDb = 0.0f;
    setOnePole(sampleRate, music.compressor.attackSeconds, music.compressor.attackCoeff);
    setOnePole(sampleRate, music.compressor.releaseSeconds, music.compressor.releaseCoeff);

    ambience.highPass.setHighPass(sampleRate, 120.0f);
    ambience.gate.openDb = -42.0f;
    ambience.gate.closeDb = -50.0f;
    ambience.gate.attackSeconds = 0.004f;
    ambience.gate.releaseSeconds = 0.050f;
    ambience.gate.floorDb = -72.0f;
    ambience.gate.level = 0.0f;
    ambience.gate.gainDb = 0.0f;
    setOnePole(sampleRate, ambience.gate.attackSeconds, ambience.gate.attackCoeff);
    setOnePole(sampleRate, ambience.gate.releaseSeconds, ambience.gate.releaseCoeff);
    setExponentialRelease(sampleRate, 0.050f, ambience.gate.releaseLevelCoeff);

    reset();
}

void StemProcessor::reset() noexcept
{
    voice.highPass.reset();
    voice.deEsserBand.reset();
    voice.presence.reset();
    voice.deEsserLevel = 0.0f;
    voice.compressor.detectorDb = -96.0f;
    voice.compressor.gainDb = 0.0f;
    music.highPass.reset();
    music.compressor.detectorDb = -96.0f;
    music.compressor.gainDb = 0.0f;
    ambience.highPass.reset();
    ambience.gate.level = 0.0f;
    ambience.gate.gainDb = 0.0f;
    for (auto& value : appliedGainDb)
        value.store(0.0f, std::memory_order_relaxed);
}

void StemProcessor::setEnabled(bool shouldBeEnabled) noexcept
{
    enabled.store(shouldBeEnabled, std::memory_order_release);
}

bool StemProcessor::isEnabled() const noexcept
{
    return enabled.load(std::memory_order_acquire);
}

float StemProcessor::dbOf(float magnitude) noexcept
{
    return magnitude > 1.0e-7f ? 20.0f * std::log10(magnitude) : -140.0f;
}

void StemProcessor::applyCompressor(Compressor& compressor, float& left, float& right) noexcept
{
    const auto input = 0.5f * (std::abs(left) + std::abs(right));
    const auto inputDb = dbOf(input);
    // Level detector, one pole in dB: attack on the way up, release on the way
    // down.
    if (inputDb > compressor.detectorDb)
        compressor.detectorDb += compressor.attackCoeff * (inputDb - compressor.detectorDb);
    else
        compressor.detectorDb += compressor.releaseCoeff * (inputDb - compressor.detectorDb);

    const auto overDb = compressor.detectorDb - compressor.thresholdDb;
    const auto targetGainDb = overDb > 0.0f ? -overDb * (1.0f - 1.0f / compressor.ratio) : 0.0f;
    if (targetGainDb < compressor.gainDb)
        compressor.gainDb += compressor.attackCoeff * (targetGainDb - compressor.gainDb);
    else
        compressor.gainDb += compressor.releaseCoeff * (targetGainDb - compressor.gainDb);

    const auto gain = std::pow(10.0f, 0.05f * (compressor.gainDb + compressor.makeupDb));
    left *= gain;
    right *= gain;
}

void StemProcessor::applyGate(Gate& gate, float& left, float& right) noexcept
{
    const auto input = std::max(std::abs(left), std::abs(right));
    // Instant attack, exponential release on the detected level so the gate
    // does not chatter as it falls.
    gate.level = input > gate.level ? input : gate.level * gate.releaseLevelCoeff;
    const auto inputDb = dbOf(gate.level);

    float targetDb;
    if (inputDb < gate.closeDb)
        targetDb = gate.floorDb;
    else if (inputDb > gate.openDb)
        targetDb = 0.0f;
    else
    {
        const auto t = (inputDb - gate.closeDb) / (gate.openDb - gate.closeDb);
        targetDb = gate.floorDb + t * (0.0f - gate.floorDb);
    }

    if (targetDb < gate.gainDb)
        gate.gainDb += gate.attackCoeff * (targetDb - gate.gainDb);
    else
        gate.gainDb += gate.releaseCoeff * (targetDb - gate.gainDb);

    const auto gain = std::pow(10.0f, 0.05f * gate.gainDb);
    left *= gain;
    right *= gain;
}

void StemProcessor::processSample(StemRole role, float& channelLeft, float& channelRight) noexcept
{
    if (!enabled.load(std::memory_order_relaxed))
        return;

    auto applied = 0.0f;
    switch (role)
    {
    case StemRole::voice:
    {
        channelLeft = voice.highPass.process(0, channelLeft);
        channelRight = voice.highPass.process(1, channelRight);

        // Gentle surge control first, then the de-esser on its own, so the
        // compressor cannot re-level the sibilance the de-esser just cut.
        applyCompressor(voice.compressor, channelLeft, channelRight);

        // De-esser: subtract a ducked copy of the sibilance band. The detector
        // follows the band envelope so only the "sss" punches get cut.
        const auto sideLeft = voice.deEsserBand.process(0, channelLeft);
        const auto sideRight = voice.deEsserBand.process(1, channelRight);
        const auto sideMag = 0.5f * (std::abs(sideLeft) + std::abs(sideRight));
        voice.deEsserLevel = sideMag > voice.deEsserLevel
            ? sideMag : voice.deEsserLevel * voice.deEsserReleaseCoeff;
        const auto sideDb = dbOf(voice.deEsserLevel);
        const auto depth = std::clamp((sideDb - deEsserThresholdDb) / deEsserRangeDb, 0.0f, 1.0f)
            * deEsserDepth;
        channelLeft -= depth * sideLeft;
        channelRight -= depth * sideRight;

        channelLeft = voice.presence.process(0, channelLeft);
        channelRight = voice.presence.process(1, channelRight);
        applied = voice.compressor.gainDb;
        break;
    }
    case StemRole::music:
    {
        channelLeft = music.highPass.process(0, channelLeft);
        channelRight = music.highPass.process(1, channelRight);
        applyCompressor(music.compressor, channelLeft, channelRight);
        applied = music.compressor.gainDb;
        break;
    }
    case StemRole::ambience:
    {
        channelLeft = ambience.highPass.process(0, channelLeft);
        channelRight = ambience.highPass.process(1, channelRight);
        applyGate(ambience.gate, channelLeft, channelRight);
        applied = ambience.gate.gainDb;
        break;
    }
    }

    appliedGainDb[static_cast<size_t>(role)].store(applied, std::memory_order_relaxed);
}

float StemProcessor::getAppliedGainDb(StemRole role) const noexcept
{
    const auto index = static_cast<int>(role);
    if (index < 0 || index >= roleCount) return 0.0f;
    return appliedGainDb[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}
} // namespace churchstream