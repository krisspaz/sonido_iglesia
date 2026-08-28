#pragma once

#include "SmartMaskingController.h"

#include <array>
#include <atomic>
#include <string>

namespace churchstream
{
enum class AutoRoutePhase : int { waitingForAudio, analysing, ready, uncertain };

struct AutoRouteSnapshot
{
    AutoRoutePhase phase = AutoRoutePhase::waitingForAudio;
    GroupRoutingConfig routes;
    std::array<float, 3> confidence {};
    int candidatePairs = 0;
    float analysedSeconds = 0.0f;
};

// Identifies three stereo stems among the first eight X32 card channels.
// It is deliberately conservative: until one stable, distinct assignment is
// supported by enough audio, the AudioEngine continues using Card 1-2.
class AutoGroupRouter final
{
public:
    static constexpr int maxCandidatePairs = 4;

    AutoGroupRouter() noexcept { reset(); }

    void prepare(double newSampleRate) noexcept;
    void reset() noexcept;
    void process(const float* const* channels, int channelCount, int sampleCount) noexcept;
    void setCandidateName(int pairIndex, const std::string& name);

    [[nodiscard]] AutoRouteSnapshot getSnapshot() const noexcept;
    [[nodiscard]] bool getResolvedRoutes(GroupRoutingConfig& destination) const noexcept;

private:
    struct Candidate
    {
        double lowState[2] {};
        double fullState[2] {};
        float level = 0.0f;
        float lowRatio = 0.0f;
        float midRatio = 0.0f;
        float highRatio = 0.0f;
        float width = 0.0f;
        float crest = 1.0f;
        float activity = 0.0f;
    };

    struct Assignment
    {
        std::array<int, 3> pair { -1, -1, -1 };
        std::array<float, 3> confidence {};
        float score = -1000.0f;
    };

    void evaluate(int pairCount) noexcept;
    [[nodiscard]] static float roleScore(const Candidate&, GroupRole, float nameHint) noexcept;
    [[nodiscard]] static float keywordScore(const std::string&, GroupRole);

    std::array<Candidate, maxCandidatePairs> candidates;
    // Channel labels arrive from the UI/OSC side while process() runs in the
    // audio callback. Keep that crossing lock-free and race-free.
    std::array<std::array<std::atomic<float>, 3>, maxCandidatePairs> nameHints;
    double sampleRate = 48000.0;
    double analysedSamples = 0.0;
    float evaluationCountdown = 0.0f;
    Assignment pending;
    float pendingSeconds = 0.0f;

    std::atomic<int> phase { static_cast<int>(AutoRoutePhase::waitingForAudio) };
    std::atomic<int> pairCountPublished { 0 };
    std::atomic<float> secondsPublished { 0.0f };
    std::array<std::atomic<int>, 3> routePairs;
    std::array<std::atomic<float>, 3> routeConfidence;
};
} // namespace churchstream
