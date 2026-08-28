#include "SmartMaskingController.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace psy = psychoacoustics;

namespace
{
// Maps each critical band onto the application zone that will actually carry
// its reduction. Built once: the band centres are compile-time constants.
const std::array<int, psy::criticalBandCount>& bandZones() noexcept
{
    static const auto zones = [] {
        std::array<int, psy::criticalBandCount> value {};
        for (int band = 0; band < psy::criticalBandCount; ++band)
        {
            const auto centre = psy::criticalBandCentresHz[static_cast<size_t>(band)];
            auto best = 0;
            auto bestDistance = 1.0e9f;
            for (int zone = 0; zone < maskingZoneCount; ++zone)
            {
                // Distance measured in Bark, not Hz: zones are perceptual.
                const auto distance = std::abs(psy::barkFromHertz(centre)
                    - psy::barkFromHertz(maskingZoneCentresHz[static_cast<size_t>(zone)]));
                if (distance < bestDistance) { bestDistance = distance; best = zone; }
            }
            value[static_cast<size_t>(band)] = best;
        }
        return value;
    }();
    return zones;
}

}

MaskingDecision SmartMaskingController::update(const GroupFeatures& voice,
                                               const GroupFeatures& music,
                                               float elapsedSeconds) noexcept
{
    const auto dt = std::clamp(elapsedSeconds, 0.01f, 1.0f);
    MaskingDecision decision;
    decision.voiceActive = voice.rmsDb > -45.0f && voice.voiceProbability >= 0.45f;

    // What the music is doing to the voice, band by band, through Schroeder's
    // spreading function rather than by looking at overlap in the same band.
    // The difference matters: a bass guitar two Bark below a vowel masks it
    // without ever sharing a band with it.
    std::array<float, psy::criticalBandCount> threshold {};
    std::array<std::array<float, psy::criticalBandCount>, psy::criticalBandCount> contribution {};
    psy::maskingThresholdAndContributions(music.bandLevelDb, threshold, contribution);
    decision.speechIntelligibility = psy::speechIntelligibilityIndex(voice.bandLevelDb, threshold);

    // Per-zone reduction, weighted by how much each band actually contributes
    // to understanding speech. Attenuating where the standard says nothing is
    // carried would cost music for no intelligibility at all.
    std::array<float, maskingZoneCount> zoneDeficit {};
    const auto shortfall = decision.voiceActive
        ? std::clamp((intelligibilityTarget - decision.speechIntelligibility)
                         / intelligibilityTarget, 0.0f, 1.0f)
        : 0.0f;

    for (int band = 0; band < psy::criticalBandCount; ++band)
    {
        const auto index = static_cast<size_t>(band);
        const auto audibility = psy::bandAudibility(voice.bandLevelDb[index], threshold[index]);
        // How far this band is from being fully audible, in dB, given the SII's
        // 30 dB audibility ramp.
        const auto deficitDb = (1.0f - audibility) * 30.0f;
        const auto weight = psy::speechImportance[index];
        if (deficitDb <= 0.0f) continue;

        // Charge the deficit to whoever caused it. Each masker band gets the
        // share of the blame that matches its share of the threshold.
        for (int masker = 0; masker < psy::criticalBandCount; ++masker)
        {
            const auto zone = static_cast<size_t>(bandZones()[static_cast<size_t>(masker)]);
            zoneDeficit[zone] += deficitDb * weight * contribution[index][static_cast<size_t>(masker)];
        }
    }

    // The shortfall says how much reduction is needed; the per-zone deficit
    // only says how to divide it up. Multiplying the two would attenuate the
    // decision twice over, and a sermon at an SII of 0.47 would be answered
    // with a fifth of a dB.
    const auto worstZoneDeficit = *std::max_element(zoneDeficit.begin(), zoneDeficit.end());

    auto confidenceSum = 0.0f;
    for (int zone = 0; zone < maskingZoneCount; ++zone)
    {
        const auto index = static_cast<size_t>(zone);
        // Share of the blame, so the zone doing the most masking gets the full
        // shortfall and the others get proportionally less.
        const auto share = worstZoneDeficit > 1.0e-6f ? zoneDeficit[index] / worstZoneDeficit : 0.0f;
        const auto severity = shortfall * share;

        persistence[index] = severity > 0.03f
            ? std::min(5.0f, persistence[index] + dt)
            : std::max(0.0f, persistence[index] - dt * 0.7f);
        const auto confidence = severity * std::clamp(persistence[index] / 1.5f, 0.0f, 1.0f);
        const auto target = confidence > 0.05f
            ? -std::min(maximumReductionDb, 0.5f + severity * maximumReductionDb) : 0.0f;
        const auto time = std::abs(target) > std::abs(currentGainDb[index]) ? 1.2f : 4.0f;
        const auto alpha = 1.0f - std::exp(-dt / time);
        currentGainDb[index] += alpha * (target - currentGainDb[index]);
        decision.musicGainDb[index] = currentGainDb[index];
        confidenceSum += confidence;
        decision.active = decision.active || currentGainDb[index] < -0.05f;
    }
    decision.confidence = std::clamp(confidenceSum / 2.0f, 0.0f, 1.0f);
    return decision;
}

void SmartMaskingController::reset() noexcept
{
    persistence.fill(0.0f);
    currentGainDb.fill(0.0f);
}
} // namespace churchstream
