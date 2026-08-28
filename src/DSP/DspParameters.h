#pragma once

#include <atomic>

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
    std::atomic<float> abMatchGainDb { 0.0f };
};
} // namespace churchstream
