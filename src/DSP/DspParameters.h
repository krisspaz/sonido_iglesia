#pragma once

#include <array>
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
    // Conservative offsets set only after an operator confirms an offline
    // Church Sound Analyst recommendation. The real-time engine remains fully
    // C++ and clamps these values before they reach a filter.
    std::atomic<float> analystMudOffsetDb { 0.0f };
    std::atomic<float> analystHarshOffsetDb { 0.0f };
    std::atomic<float> analystSibilanceOffsetDb { 0.0f };

    std::atomic<bool> rumbleEnabled { true };
    // Slow EQ towards a fixed target curve (ToneMatch). Automatic, so it stands
    // down in MANUAL like the Smart Engine does.
    std::atomic<bool> toneMatchEnabled { true };
    std::atomic<bool> adaptiveEqEnabled { true };
    std::atomic<bool> compressorEnabled { true };
    std::atomic<bool> saturationEnabled { true };
    // Dynamic EQ and de-esser. Separate from the adaptive EQ because they work
    // on completely different timescales: the Smart Engine decides tone over
    // seconds, these two react inside a syllable.
    std::atomic<bool> dynamicEqEnabled { true };
    std::atomic<bool> deEsserEnabled { true };
    std::atomic<bool> limiterEnabled { true };
    // Optional three-band wet limiter. It uses the existing 0-500 Hz,
    // 500-4 kHz and >4 kHz crossover groups; the full-band true-peak limiter
    // remains the final safety net. Ceilings are linear amplitudes and are
    // constrained by the engine to never sit below the global ceiling.
    std::atomic<bool> multibandLimiterEnabled { false };
    std::atomic<bool> multibandLimiterLowEnabled { true };
    std::atomic<bool> multibandLimiterMidEnabled { true };
    std::atomic<bool> multibandLimiterHighEnabled { true };
    std::atomic<float> multibandLimiterLowCeiling { 0.95f };
    std::atomic<float> multibandLimiterMidCeiling { 0.95f };
    std::atomic<float> multibandLimiterHighCeiling { 0.95f };
    std::atomic<float> multibandLimiterLowAttackMs { 1.0f };
    std::atomic<float> multibandLimiterMidAttackMs { 1.0f };
    std::atomic<float> multibandLimiterHighAttackMs { 1.0f };
    std::atomic<float> multibandLimiterLowReleaseMs { 180.0f };
    std::atomic<float> multibandLimiterMidReleaseMs { 120.0f };
    std::atomic<float> multibandLimiterHighReleaseMs { 70.0f };
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
    // Width for a mono console feed (MonoSpread). Only engages when the input
    // is mono, and never changes the mono sum.
    std::atomic<bool> monoSpreadEnabled { true };
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
    std::atomic<float> multibandLimiterLowReductionDb { 0.0f };
    std::atomic<float> multibandLimiterMidReductionDb { 0.0f };
    std::atomic<float> multibandLimiterHighReductionDb { 0.0f };
    std::atomic<float> truePeakEstimate { 0.0f };
    std::atomic<float> appliedOutputGainDb { 0.0f };
    // What the dynamic sections are actually doing right now, so the operator
    // can tell a de-esser that is working from one that is sitting on the whole
    // top end.
    std::atomic<float> deEsserReductionDb { 0.0f };
    // ToneMatch gains: low shelf 120 Hz, 280 Hz, 3.2 kHz, 7 kHz, high shelf
    // 12 kHz. Published so the Smart Engine can judge the mix without them.
    std::array<std::atomic<float>, 5> toneMatchGainDb {};
    std::atomic<float> dynamicEqReductionDb { 0.0f };
    std::atomic<float> compressorMakeupDb { 0.0f };
    std::atomic<float> broadcastLevelGainDb { 0.0f };
    std::atomic<float> abMatchGainDb { 0.0f };
    // Inter-channel correlation of the processed programme, and the width the
    // engine actually applied after coherence safety.
    std::atomic<float> programmeCorrelation { 1.0f };
    std::atomic<float> appliedStereoWidth { 1.0f };
    // 0 when the input is stereo or MonoSpread is off, 1 when a mono feed is
    // being fully spread.
    std::atomic<float> monoSpreadWeight { 0.0f };
    // Kalman estimate of programme level, and whether the leveler gate is open.
    // A closed gate means the leveler is deliberately doing nothing.
    std::atomic<float> programmeLevelDb { -100.0f };
    std::atomic<bool> levelerGateOpen { false };
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
