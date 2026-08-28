#pragma once

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

struct GroupFeatures
{
    std::array<float, 5> bandEnergy {};
    float rmsDb = -100.0f;
    float voiceProbability = 0.0f;
};

struct MaskingDecision
{
    std::array<float, 5> musicGainDb {};
    float confidence = 0.0f;
    bool voiceActive = false;
    // The controller decided a reduction is warranted.
    bool active = false;
    // The reduction actually reached the audio. It stays false while masking is
    // disabled, which is the advisory mode: the operator can watch a whole
    // service to see what masking would have done before switching it on.
    bool applied = false;
};

// Produces band-selective gain targets for the MUSIC group. It never touches
// the voice group and is inert until valid, separately routed X32 groups exist.
class SmartMaskingController final
{
public:
    MaskingDecision update(const GroupFeatures& voice, const GroupFeatures& music,
                           float elapsedSeconds) noexcept;
    void reset() noexcept;

private:
    std::array<float, 5> persistence {};
    std::array<float, 5> currentGainDb {};
};
} // namespace churchstream
