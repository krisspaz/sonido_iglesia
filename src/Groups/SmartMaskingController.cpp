#include "SmartMaskingController.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
MaskingDecision SmartMaskingController::update(const GroupFeatures& voice,
                                               const GroupFeatures& music,
                                               float elapsedSeconds) noexcept
{
    const auto dt = std::clamp(elapsedSeconds, 0.01f, 1.0f);
    MaskingDecision decision;
    decision.voiceActive = voice.rmsDb > -45.0f && voice.voiceProbability >= 0.45f;
    auto confidenceSum = 0.0f;

    for (size_t band = 0; band < currentGainDb.size(); ++band)
    {
        // Intelligibility lives mainly from 500 Hz to 8 kHz. Low/high bands
        // remain untouched so the music does not appear to disappear.
        const auto relevant = band == 2 || band == 3;
        const auto overlap = relevant && decision.voiceActive
            ? std::sqrt(std::max(0.0f, voice.bandEnergy[band] * music.bandEnergy[band])) : 0.0f;
        const auto dominance = music.bandEnergy[band]
            / std::max(0.001f, voice.bandEnergy[band] + music.bandEnergy[band]);
        const auto severity = std::clamp((overlap - 0.08f) * 7.0f * dominance
                                             * voice.voiceProbability,
                                         0.0f, 1.0f);
        persistence[band] = severity > 0.08f
            ? std::min(5.0f, persistence[band] + dt)
            : std::max(0.0f, persistence[band] - dt * 0.7f);
        const auto confidence = severity * std::clamp(persistence[band] / 1.5f, 0.0f, 1.0f);
        const auto target = confidence > 0.55f ? -std::min(2.5f, 0.5f + severity * 2.0f) : 0.0f;
        const auto time = std::abs(target) > std::abs(currentGainDb[band]) ? 1.2f : 4.0f;
        const auto alpha = 1.0f - std::exp(-dt / time);
        currentGainDb[band] += alpha * (target - currentGainDb[band]);
        decision.musicGainDb[band] = currentGainDb[band];
        confidenceSum += confidence;
        decision.active = decision.active || currentGainDb[band] < -0.05f;
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
