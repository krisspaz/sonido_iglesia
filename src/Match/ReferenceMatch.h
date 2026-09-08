#pragma once

#include "Analysis/AnalysisEngine.h"
#include "Analysis/Psychoacoustics.h"

#include <array>
#include <juce_core/juce_core.h>

// Reference Match: captures what a known-good recording sounds like (its tonal
// balance in the 21 ANSI critical bands plus loudness, dynamic range, crest and
// stereo width) and then compares the live stream against it, in words a
// volunteer media director understands. The engine only observes; it never
// touches audio, like the Smart Masking controller before the operator turns
// it on.
namespace churchstream
{
constexpr int referenceZoneCount = 4;

// One operator-facing frequency zone, backed by a contiguous slice of the
// critical bands.
struct ReferenceZoneInfo
{
    int firstBand = 0;
    int bandCount = 0;
    const char* name = nullptr;
    const char* range = nullptr;
};

// The four zones cover every critical band, in the words an operator uses:
// graves, cuerpo, presencia and brillo.
constexpr std::array<ReferenceZoneInfo, referenceZoneCount> referenceZones { {
    { 0, 5, "graves", "150-570 Hz" },
    { 5, 7, "cuerpo", "700 Hz-1.9 kHz" },
    { 12, 5, "presencia", "2.2-4 kHz" },
    { 17, 4, "brillo", "4.8-8.5 kHz" },
} };

[[nodiscard]] inline int referenceZoneForBand(int band) noexcept
{
    for (int zone = 0; zone < referenceZoneCount; ++zone)
    {
        const auto& info = referenceZones[static_cast<size_t>(zone)];
        if (band >= info.firstBand && band < info.firstBand + info.bandCount)
            return zone;
    }
    return 3;
}

// The frozen fingerprint of a known-good stream. Captured with the same
// AnalysisEngine measurement the live path uses, so a difference is a real
// difference and not a measurement mismatch.
struct ReferenceProfile
{
    // Per-critical-band level, as measured by the offline pass. Compared
    // against the live bands after each side is shifted to its own loudness,
    // so the comparison is about shape, not about volume.
    std::array<float, psychoacoustics::criticalBandCount> bandLevelDb {};
    float lufsIntegrated = -100.0f;
    float loudnessRange = 0.0f;
    float crestFactorDb = 0.0f;
    float stereoWidth = 0.0f;
    float durationSeconds = 0.0f;
    juce::String sourceName;
    bool valid = false;

    [[nodiscard]] bool save(const juce::File& destination) const;
    static ReferenceProfile load(const juce::File& source);
    static ReferenceProfile capture(const juce::File& audioFile, juce::String& error);
    static ReferenceProfile empty();

    // Where reference profiles live, mirroring the PresetManager conventions.
    [[nodiscard]] static juce::File defaultDirectory();
};

// The live-vs-reference verdict. Positive differences mean the live stream has
// more than the reference.
struct ReferenceComparison
{
    bool active = false;
    // False until the live stream has real audio to trust.
    bool meaningful = false;
    float liveSeconds = 0.0f;
    // live - reference per critical band, after both sides are normalised to
    // their own loudness. Band 0 is graves, band 20 is brillo.
    std::array<float, psychoacoustics::criticalBandCount> bandDifferenceDb {};
    // Same difference, averaged per operator zone.
    std::array<float, referenceZoneCount> zoneDifferenceDb {};
    float loudnessDifferenceDb = 0.0f;
    float crestDifferenceDb = 0.0f;
    float widthDifference = 0.0f;
    float liveLufsShortTerm = -100.0f;
    juce::String title;
    juce::StringArray lines;
};

// Polls the live analysis at a fixed low rate while a profile is loaded and
// publishes the comparison for the UI. Captures are run on this same thread so
// a large reference file does not block the interface.
class ReferenceMatchEngine final : private juce::Thread
{
public:
    explicit ReferenceMatchEngine(AnalysisEngine& analysisToUse);
    ~ReferenceMatchEngine() override;

    void start();
    void stop();

    // Capture runs on the engine thread; completion is published to
    // getCaptureState. On success the captured profile becomes the active one.
    void captureFromFile(const juce::File& audioFile);
    juce::Result loadProfile(const juce::File& profileFile);
    void clearProfile();
    // Directly install a profile (used by the UI and the tests).
    void setProfile(const ReferenceProfile& profile);

    [[nodiscard]] ReferenceProfile getProfile() const;
    [[nodiscard]] ReferenceComparison getComparison() const;

    struct CaptureState
    {
        bool running = false;
        bool complete = false;
        bool ok = false;
        juce::String file;
        juce::String error;
    };
    [[nodiscard]] CaptureState getCaptureState() const;

    // Deterministic entry point for tests: runs exactly the code the thread
    // runs for one snapshot, without threading or the wall clock.
    void processSnapshotForTesting(const AnalysisSnapshot& snapshot);

    // The pure live-vs-reference computation, shared by the thread and the
    // tests.
    static ReferenceComparison compare(const ReferenceProfile& profile,
                                       const SignalMetrics& live, double sampleRate,
                                       float liveSeconds) noexcept;

private:
    void run() override;
    void update(const AnalysisSnapshot& snapshot);

    AnalysisEngine& analysis;
    mutable juce::CriticalSection profileLock;
    ReferenceProfile profile;
    mutable juce::CriticalSection comparisonLock;
    ReferenceComparison comparison;
    mutable juce::CriticalSection captureLock;
    CaptureState captureState;
    juce::String pendingCaptureFile;
    uint64_t previousFrames = 0;
    float liveSeconds = 0.0f;
};
} // namespace churchstream