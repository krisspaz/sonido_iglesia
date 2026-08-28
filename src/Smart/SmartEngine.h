#pragma once

#include "Analysis/AnalysisEngine.h"
#include "DSP/ProcessingEngine.h"
#include "SmartTypes.h"

#include <array>
#include <juce_core/juce_core.h>

namespace churchstream
{
class SmartEngine final : private juce::Thread
{
public:
    // Low, low-mid, presence, high-mid, sibilance and air. Each one owns an
    // independent closed loop and its own learned effectiveness.
    static constexpr size_t tonalCorrections = 6;

    SmartEngine(AnalysisEngine& analysisToUse, ProcessingEngine& processingToUse,
                juce::File profileFileToUse = {}, juce::String churchNameToUse = "Mi Iglesia");
    ~SmartEngine() override;

    void start();
    void stop();
    void startAutoTune();
    void setUpdateRateHz(int rate) noexcept;
    void setScene(SmartScene scene) noexcept;
    void setChurchName(const juce::String& name);
    void requestSafetyRollback() noexcept;
    [[nodiscard]] SmartState getState() const;

    // Deterministic entry point for tests and offline validation. It runs the
    // same decision code as the background thread and never touches audio.
    void processSnapshotForTesting(const AnalysisSnapshot& snapshot, float elapsedSeconds);

private:
    struct Baseline
    {
        std::array<float, 5> bands { 0.12f, 0.24f, 0.35f, 0.23f, 0.06f };
        float loudness = -14.0f;
        float crest = 10.0f;
        float centroid = 2200.0f;
        float stereoWidth = 0.55f;
        float sibilance = 0.055f;
        bool ready = false;
    };

    struct Accumulator
    {
        std::array<double, 5> bands {};
        double loudness = 0.0;
        double crest = 0.0;
        double centroid = 0.0;
        double stereoWidth = 0.0;
        double sibilance = 0.0;
        int count = 0;
    };

    struct FeedbackLoop
    {
        bool evaluating = false;
        float startingSeverity = 0.0f;
        float startingScore = 0.0f;
        float target = 0.0f;
        float elapsed = 0.0f;
        float cooldown = 0.0f;
        bool suppressCorrection = false;
        juce::String lastResult;
        bool lastRollback = false;
    };

    void run() override;
    void update(const AnalysisSnapshot& snapshot, float elapsedSeconds);
    void updateAutoTune(const SignalMetrics& metrics, float elapsedSeconds);
    void decide(const AnalysisSnapshot& snapshot, float elapsedSeconds, SmartState& next);
    QualityScores calculateQuality(const AnalysisSnapshot& snapshot, const SmartState& previous,
                                   float elapsedSeconds) noexcept;
    void buildProblems(const AnalysisSnapshot& snapshot, SmartState& next);
    void evaluateClosedLoop(const std::array<float, tonalCorrections>& severity,
                            std::array<float, tonalCorrections>& tonalTargets, float elapsedSeconds,
                            SmartState& next);
    void loadProfile();
    void saveProfile();
    void publishTargets(const std::array<float, tonalCorrections>& tonalTargets,
                        float rumbleCutoff, float compression, float loudness,
                        float elapsedSeconds);
    static MixProfile detectProfile(const Baseline& baseline) noexcept;
    static float confidenceFromPersistence(float severity, float seconds, float requiredSeconds) noexcept;
    static float spectrumEnergy(const SignalMetrics& metrics, double sampleRate,
                                float lowHz, float highHz) noexcept;
    static void addAction(SmartState& state, juce::String name, juce::String reason,
                          float amount, float confidence, int priority, float frequencyHz = 0.0f,
                          juce::String result = {}, bool rolledBack = false);
    static void addProblem(SmartState& state, juce::String name, juce::String detail,
                           float severity, bool warning);
    static float smooth(float current, float target, float elapsedSeconds,
                        float attackSeconds, float releaseSeconds) noexcept;

    AnalysisEngine& analysis;
    ProcessingEngine& processing;
    mutable juce::CriticalSection stateLock;
    SmartState state;
    Baseline baseline;
    Accumulator autoTuneAccumulator;
    std::atomic<bool> autoTuneRequested { false };
    std::atomic<int> updateRate { 8 };
    std::atomic<int> requestedScene { static_cast<int>(SmartScene::autoDetect) };
    std::atomic<bool> safetyRollbackRequested { false };
    float autoTuneElapsed = 0.0f;
    std::array<float, 9> persistence {};
    // 0-5 tonal corrections, 6 rumble cutoff, 7 loudness gain.
    std::array<float, tonalCorrections + 2> currentTargets { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 0.0f };
    std::array<FeedbackLoop, tonalCorrections> feedback;
    std::array<float, tonalCorrections> learnedEffectiveness { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    std::array<float, 5> previousBandEnergy {};
    float previousLoudness = -14.0f;
    juce::File profileFile;
    juce::String churchName { "Mi Iglesia" };
    std::atomic<bool> profileDirty { false };
    float profileSaveElapsed = 0.0f;
};
} // namespace churchstream
