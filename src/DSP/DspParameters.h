#pragma once

#include <atomic>
#include <cstdint>

namespace churchstream
{
enum class OperatingMode : int
{
    autoMode = 0,
    safe,
    manual
};

struct DspParameters final
{
    std::atomic<bool> smartProcessing { true };
    std::atomic<bool> bypass { false };
    std::atomic<bool> abProcessed { true };
    // Loudness-matched A/B. Without it the processed side wins every comparison
    // simply because it is louder. Bypass is never matched: it must stay a true
    // safety path.
    std::atomic<bool> abLoudnessMatch { true };
    std::atomic<int> operatingMode { static_cast<int>(OperatingMode::safe) };

    std::atomic<float> clean { 0.50f };
    std::atomic<float> punch { 0.50f };
    std::atomic<float> clarity { 0.50f };
    std::atomic<float> dynamics { 0.50f };
    std::atomic<float> warmth { 0.35f };
    std::atomic<float> loudnessTarget { -14.0f };

    std::atomic<bool> rumbleEnabled { true };
    std::atomic<bool> adaptiveEqEnabled { true };
    std::atomic<bool> compressorEnabled { true };
    std::atomic<bool> saturationEnabled { true };
    std::atomic<bool> limiterEnabled { true };
    // Stereo program leveler is enabled by the application setting. It is
    // separate from Smart Engine so it can safely stabilise a plain X32 L/R
    // stream even when the source has no isolated stems.
    std::atomic<bool> broadcastLevelerEnabled { false };
    // Panic switch. Holds the engine on the dry safety path with the same 10 ms
    // crossfade the watchdog uses, so an operator can leave the processed path
    // during a service without a click and without stopping audio.
    std::atomic<bool> forceFailsafe { false };
    // Mono compatibility. Most of the stream is heard on a single phone
    // speaker, where anything sitting in the Side channel cancels instead of
    // adding. Low frequencies are collapsed first because that is where phase
    // error costs the most energy and where the image is least audible anyway.
    std::atomic<bool> monoCompatibilityEnabled { true };
    std::atomic<float> bassMonoFrequencyHz { 120.0f };
    // Correlation-driven width safety. Enabled separately from bass mono: it
    // reacts to the programme rather than applying a fixed rule.
    std::atomic<bool> phaseCoherenceEnabled { true };
};

struct AdaptiveTargets final
{
    std::atomic<float> rumbleCutoffHz { 20.0f };
    std::atomic<float> lowGainDb { 0.0f };
    std::atomic<float> mudGainDb { 0.0f };
    std::atomic<float> clarityGainDb { 0.0f };
    std::atomic<float> harshGainDb { 0.0f };
    std::atomic<float> sibilanceGainDb { 0.0f };
    std::atomic<float> highGainDb { 0.0f };
    std::atomic<float> compressionDb { 0.0f };
    std::atomic<float> loudnessGainDb { 0.0f };
    std::atomic<float> stereoWidth { 1.0f };
    std::atomic<float> stereoBalanceDb { 0.0f };
};

struct DspMetrics final
{
    std::atomic<float> compressorGainReductionDb { 0.0f };
    std::atomic<float> limiterGainReductionDb { 0.0f };
    std::atomic<float> truePeakEstimate { 0.0f };
    std::atomic<float> appliedOutputGainDb { 0.0f };
    std::atomic<float> broadcastLevelGainDb { 0.0f };
    std::atomic<float> abMatchGainDb { 0.0f };
    // Inter-channel correlation of the processed programme, and the width the
    // engine actually applied after coherence safety.
    std::atomic<float> programmeCorrelation { 1.0f };
    std::atomic<float> appliedStereoWidth { 1.0f };
    // DSP watchdog. A non-finite or divergent sample must never reach a live
    // stream, so the engine crossfades to the dry safety path instead. These
    // are reported so the operator and the SafetyController can see that the
    // processed path failed, rather than silently listening to a bypass.
    std::atomic<bool> failsafeActive { false };
    std::atomic<uint32_t> failsafeEngagements { 0 };
    // Samples arriving already broken from the driver. They are replaced with
    // silence before they can poison any recursive state, and counted here
    // because the fault is upstream of the DSP, not caused by it.
    std::atomic<uint32_t> nonFiniteInputSamples { 0 };
};
} // namespace churchstream
