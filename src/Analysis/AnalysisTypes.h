#pragma once

#include <array>
#include <cstdint>

namespace churchstream
{
constexpr int fftOrder = 11;
constexpr int fftSize = 1 << fftOrder;
constexpr int spectrumBins = fftSize / 2 + 1;

enum class MixContext : int
{
    quiet,
    speech,
    music,
    fullBand,
    denseMusic,
    ambience,
    worshipSoft,
    soloVocal
};

struct SignalMetrics
{
    float peakDb = -100.0f;
    float truePeakDbtp = -100.0f;
    float rmsDb = -100.0f;
    float lufsMomentary = -100.0f;
    float lufsShortTerm = -100.0f;
    float lufsIntegrated = -100.0f;
    float loudnessRange = 0.0f;
    float crestFactorDb = 0.0f;
    float dynamicRangeDb = 0.0f;
    float stereoCorrelation = 1.0f;
    float stereoWidth = 0.0f;
    float leftRightImbalanceDb = 0.0f;
    float spectralCentroidHz = 0.0f;
    float transientDensity = 0.0f;
    std::array<float, 5> bandEnergy { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, spectrumBins> spectrumDb {};
};

struct AnalysisSnapshot
{
    SignalMetrics input;
    SignalMetrics processed;
    MixContext context = MixContext::quiet;
    double sampleRate = 0.0;
    uint64_t analyzedFrames = 0;
    uint64_t droppedInputSamples = 0;
    uint64_t droppedOutputSamples = 0;
    double updateRateHz = 0.0;
};
} // namespace churchstream
