#include "SafetyController.h"

#include <algorithm>

namespace churchstream
{
SafetyState SafetyController::evaluate(const SafetyInput& input, float elapsedSeconds)
{
    const auto dt = std::clamp(elapsedSeconds, 0.05f, 2.0f);
    SafetyState next;
    rollbackCooldown = std::max(0.0f, rollbackCooldown - dt);

    disconnectedSeconds = !input.x32Connected ? disconnectedSeconds + dt : 0.0f;
    silenceSeconds = input.streamActive && input.analysis.processed.rmsDb < -65.0f
        ? silenceSeconds + dt : 0.0f;
    clippingSeconds = input.analysis.input.peakDb > -0.2f ? clippingSeconds + dt : 0.0f;
    highCpuSeconds = input.systemCpuPercent >= 80.0 ? highCpuSeconds + dt : 0.0f;
    badPhaseSeconds = input.analysis.processed.stereoCorrelation < -0.25f
        ? badPhaseSeconds + dt : 0.0f;
    excessiveProcessingSeconds = input.compressorReductionDb + input.limiterReductionDb > 10.0f
        ? excessiveProcessingSeconds + dt : 0.0f;

    if (!input.audioRunning)
        addEvent(next, "Audio stopped", "Keep outputs silent and reconnect device", true);
    if (disconnectedSeconds >= 2.0f)
    {
        next.requestX32Reconnect = true;
        addEvent(next, "X32 disconnected", "Reconnect when the stream is stopped", true);
    }
    if (silenceSeconds >= 5.0f)
        addEvent(next, "Unexpected stream silence", "Keep limiter active and alert operator", true);
    if (clippingSeconds >= 1.0f)
    {
        next.forceLimiter = true;
        addEvent(next, "Input clipping", "Protect output; reduce gain at X32", true);
    }
    if (highCpuSeconds >= 2.0f)
    {
        next.reduceSecondaryWork = true;
        addEvent(next, "High CPU", "Reduce UI and analysis rate before DSP", false);
    }
    if (input.streamActive && (!input.obsConnected || !input.obsAudioConfigured))
    {
        next.requestObsReconnect = !input.obsConnected;
        addEvent(next, "OBS audio path unavailable", "Reconnect and repair local audio source", true);
    }
    if (input.xruns > previousXruns || input.analysisDrops > previousDrops)
    {
        next.reduceSecondaryWork = true;
        addEvent(next, "Realtime deadline missed", "Reduce secondary work and preserve DSP", true);
    }
    // The first evaluation only records the counters it finds. Without this the
    // controller would report a fault on startup whenever the engine had
    // already been running, for instance after a device change.
    const auto newFailsafes = watchdogBaselineReady
        && input.dspFailsafeEngagements > previousFailsafeEngagements;
    const auto newInputFaults = watchdogBaselineReady
        && input.nonFiniteInputSamples > previousNonFiniteInputSamples;
    failsafeNoticeSeconds = newFailsafes || input.dspFailsafeActive
        ? 30.0f : std::max(0.0f, failsafeNoticeSeconds - dt);
    inputFaultNoticeSeconds = newInputFaults
        ? 30.0f : std::max(0.0f, inputFaultNoticeSeconds - dt);

    if (failsafeNoticeSeconds > 0.0f)
    {
        addEvent(next, "DSP failsafe engaged",
                 "Processed path replaced by dry audio; rollback and inspect", true);
        // Whatever diverged was most likely an adaptive correction, so undo
        // them rather than letting the engine walk straight back into it.
        if (rollbackCooldown <= 0.0f)
        {
            next.requestSmartRollback = true;
            rollbackCooldown = 15.0f;
        }
    }
    if (inputFaultNoticeSeconds > 0.0f)
        addEvent(next, "Broken input samples",
                 "Driver delivered non-finite audio; check device and sample rate", true);
    if (badPhaseSeconds >= 3.0f)
        addEvent(next, "Phase risk", "Narrow stereo image gradually", false);
    if (excessiveProcessingSeconds >= 2.0f)
    {
        addEvent(next, "Excessive processing", "Rollback adaptive corrections", true);
        if (rollbackCooldown <= 0.0f)
        {
            next.requestSmartRollback = true;
            rollbackCooldown = 15.0f;
        }
    }

    previousXruns = input.xruns;
    previousDrops = input.analysisDrops;
    previousFailsafeEngagements = input.dspFailsafeEngagements;
    previousNonFiniteInputSamples = input.nonFiniteInputSamples;
    watchdogBaselineReady = true;
    next.healthy = next.eventCount == 0;
    state = next;
    return state;
}

void SafetyController::addEvent(SafetyState& value, juce::String name,
                                juce::String response, bool critical)
{
    if (value.eventCount >= static_cast<int>(value.events.size())) return;
    auto& event = value.events[static_cast<size_t>(value.eventCount++)];
    event.name = std::move(name);
    event.response = std::move(response);
    event.critical = critical;
}
} // namespace churchstream
