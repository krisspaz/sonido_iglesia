#pragma once

#include "Analysis/AnalysisEngine.h"
#include "DSP/ProcessingEngine.h"
#include "Safety/SafetyController.h"
#include "Smart/SmartEngine.h"

#include <ebur128.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

namespace churchstream
{
struct OfflineMetrics
{
    float lufsIntegrated = -100.0f;
    float peakDb = -100.0f;
    float truePeakDbtp = -100.0f;
    float rmsDb = -100.0f;
    float dynamicRangeDb = 0.0f;
};

struct OfflineEvent
{
    double timeSeconds = 0.0;
    juce::String action;
    float frequencyHz = 0.0f;
    float amountDb = 0.0f;
    float confidence = 0.0f;
    float mixScore = 0.0f;
    juce::String result;
    bool rolledBack = false;
};

struct OfflineReport
{
    double durationSeconds = 0.0;
    float averageScore = 0.0f;
    float minimumScore = 100.0f;
    int corrections = 0;
    int rollbacks = 0;
    int improved = 0;
    int retained = 0;
    juce::String detectedProfile;
    bool baselineLearned = false;
    // Playback gain applied to each rendered file so the A/B is decided by the
    // processing and not by the level.
    float matchGainOriginalDb = 0.0f;
    float matchGainProcessedDb = 0.0f;
    std::array<int, 9> problemSeconds {};
    std::array<juce::String, 9> problemNames;
    int problemCount = 0;
    std::vector<OfflineEvent> events;
};

struct OfflineResult
{
    bool running = false;
    bool complete = false;
    float progress = 0.0f;
    juce::String stage;
    juce::String sourceName;
    juce::String error;
    juce::File processedFile;
    juce::File originalFile;
    juce::File reportFile;
    OfflineMetrics original;
    OfflineMetrics processed;
    OfflineReport report;
};

// Offline simulation of a recorded service. It drives the real Smart Engine,
// the real Safety Controller and the real DSP chain from sample counts instead
// of the wall clock, so a given file always yields the same decisions.
class OfflineProcessor final : private juce::Thread
{
public:
    OfflineProcessor();
    ~OfflineProcessor() override;

    bool startProcessing(const juce::File& source, const DspParameters& parameters,
                         const juce::File& churchProfile = {});
    void stop();
    [[nodiscard]] OfflineResult getResult() const;

private:
    struct ParameterCopy
    {
        float clean = 0.5f, punch = 0.5f, clarity = 0.5f, dynamics = 0.5f, warmth = 0.35f;
        float loudnessTarget = -14.0f;
        int operatingMode = 1;
        bool rumble = true, eq = true, compressor = true, saturation = true, limiter = true;
    };

    struct MetricAccumulator
    {
        ebur128_state* loudness = nullptr;
        double sumSquares = 0.0;
        uint64_t samples = 0;
        float peak = 0.0f;
    };

    void run() override;
    bool renderMatchedPair(const juce::File& processedSource, juce::AudioFormatManager& formats,
                           const juce::File& originalTarget, const juce::File& processedTarget,
                           float originalGainDb, float processedGainDb);
    static bool renderWithGain(juce::AudioFormatReader& reader, const juce::File& target, float gainDb);
    void writeReport(const juce::File& target, const OfflineResult& value) const;
    static void addMetrics(MetricAccumulator&, const juce::AudioBuffer<float>&, int start, int count,
                           std::vector<float>& interleaved);
    static OfflineMetrics finishMetrics(MetricAccumulator&);
    void setError(const juce::String& message);
    void setStage(const juce::String& stage, float progress);

    mutable juce::CriticalSection resultLock;
    OfflineResult result;
    juce::File sourceFile;
    juce::File profileSource;
    ParameterCopy parameterCopy;
};
} // namespace churchstream
