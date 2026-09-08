#pragma once

#include "Analysis/AnalysisTypes.h"

#include <array>
#include <cstdint>
#include <juce_core/juce_core.h>

namespace churchstream
{
struct SafetyInput
{
    bool audioRunning = false;
    bool x32Connected = false;
    bool obsConnected = false;
    bool obsAudioConfigured = false;
    bool streamActive = false;
    double systemCpuPercent = 0.0;
    int xruns = 0;
    uint64_t analysisDrops = 0;
    float compressorReductionDb = 0.0f;
    float limiterReductionDb = 0.0f;
    // DSP watchdog state, read from DspMetrics. A failsafe engagement means the
    // processed path produced something unusable and the engine fell back to
    // dry audio: the stream survived, but the operator has to know.
    bool dspFailsafeActive = false;
    uint32_t dspFailsafeEngagements = 0;
    uint32_t nonFiniteInputSamples = 0;
    AnalysisSnapshot analysis;
};

struct SafetyEvent
{
    juce::String name;
    juce::String response;
    bool critical = false;
};

struct SafetyState
{
    bool healthy = true;
    bool reduceSecondaryWork = false;
    bool requestX32Reconnect = false;
    bool requestObsReconnect = false;
    bool requestSmartRollback = false;
    bool forceLimiter = false;
    std::array<SafetyEvent, 8> events;
    int eventCount = 0;
};

class SafetyController final
{
public:
    SafetyState evaluate(const SafetyInput& input, float elapsedSeconds);
    [[nodiscard]] SafetyState getState() const { return state; }

private:
    static void addEvent(SafetyState&, juce::String name, juce::String response, bool critical);

    SafetyState state;
    float disconnectedSeconds = 0.0f;
    float silenceSeconds = 0.0f;
    float clippingSeconds = 0.0f;
    float highCpuSeconds = 0.0f;
    float badPhaseSeconds = 0.0f;
    float excessiveProcessingSeconds = 0.0f;
    int previousXruns = 0;
    uint64_t previousDrops = 0;
    float rollbackCooldown = 0.0f;
    // The failsafe crossfade lasts milliseconds but the fault it reports is
    // worth a visible warning, so the event is held long enough to be seen.
    float failsafeNoticeSeconds = 0.0f;
    float inputFaultNoticeSeconds = 0.0f;
    uint32_t previousFailsafeEngagements = 0;
    uint32_t previousNonFiniteInputSamples = 0;
    bool watchdogBaselineReady = false;
};
} // namespace churchstream
