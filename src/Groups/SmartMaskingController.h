#pragma once

#include "Analysis/Psychoacoustics.h"

#include <array>
#include <atomic>

namespace churchstream
{
enum class GroupRole : int { voice, music, ambience };

struct GroupRoute
{
    int leftChannel = -1;
    int rightChannel = -1;
    [[nodiscard]] bool validFor(int availableChannels) const noexcept
    {
        return leftChannel >= 0 && rightChannel >= 0 && leftChannel != rightChannel
            && leftChannel < availableChannels && rightChannel < availableChannels;
    }
};

struct GroupRoutingConfig
{
    GroupRoute voice { 0, 1 };
    GroupRoute music { 2, 3 };
    GroupRoute ambience { 4, 5 };
};

// Masking is decided on the 21 ANSI S3.5 critical bands, because that is the
// resolution the spreading function is defined on. It is applied on four wide
// zones: band-selective ducking at 1 Bark resolution modulates the music fast
// enough to be heard as an artefact, and the extra precision buys nothing once
// the reduction is only a couple of dB.
constexpr int maskingZoneCount = 4;
constexpr std::array<float, maskingZoneCount> maskingZoneCentresHz {
    750.0f, 1500.0f, 3000.0f, 5000.0f
};

struct GroupFeatures
{
    // Absolute band levels in dBFS. The masking model works on levels, not on
    // the normalised proportions the spectrum display uses: two mixes with the
    // same shape and 20 dB between them do not mask each other equally.
    std::array<float, psychoacoustics::criticalBandCount> bandLevelDb {};
    float rmsDb = -100.0f;
    float voiceProbability = 0.0f;

    GroupFeatures() noexcept { bandLevelDb.fill(-100.0f); }
};

struct MaskingDecision
{
    std::array<float, maskingZoneCount> musicGainDb {};
    // Speech Intelligibility Index of the voice against the masking threshold
    // the music projects onto it. This is the number the decision is made on,
    // and it is published so the operator can see why.
    float speechIntelligibility = 1.0f;
    float confidence = 0.0f;
    bool voiceActive = false;
    // The controller decided a reduction is warranted.
    bool active = false;
    // The reduction actually reached the audio. It stays false while masking is
    // disabled, which is the advisory mode: the operator can watch a whole
    // service to see what masking would have done before switching it on.
    bool applied = false;
};

// Produces zone gain targets for the MUSIC group from a Zwicker/Schroeder
// masking model. It never touches the voice group and is inert until valid,
// separately routed X32 groups exist.
class SmartMaskingController final
{
public:
    // Below this the preaching is being lost and the music has to give way.
    static constexpr float intelligibilityTarget = 0.75f;
    static constexpr float maximumReductionDb = 4.0f;

    MaskingDecision update(const GroupFeatures& voice, const GroupFeatures& music,
                           float elapsedSeconds) noexcept;
    void reset() noexcept;

private:
    std::array<float, maskingZoneCount> persistence {};
    std::array<float, maskingZoneCount> currentGainDb {};
};
} // namespace churchstream
