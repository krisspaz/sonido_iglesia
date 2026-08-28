#pragma once

#include <array>
#include <juce_audio_formats/juce_audio_formats.h>

namespace churchstream
{
struct RoomBand
{
    float centreHz = 0.0f;
    float levelDb = 0.0f;
};

struct RoomRecommendation
{
    float frequencyHz = 0.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
    juce::String reason;
};

struct RoomResult
{
    static constexpr int bandCapacity = 31;
    static constexpr int recommendationCapacity = 6;

    bool running = false;
    bool complete = false;
    float progress = 0.0f;
    juce::String sourceName;
    juce::String error;
    juce::String measurementType;
    std::array<RoomBand, bandCapacity> bands;
    int bandCount = 0;
    bool rt60Available = false;
    float rt60Seconds = 0.0f;
    float lowTiltDb = 0.0f;
    float highTiltDb = 0.0f;
    std::array<RoomRecommendation, recommendationCapacity> recommendations;
    int recommendationCount = 0;
    juce::File reportFile;
};

// Measurement-microphone analysis of the room, kept completely separate from
// the streaming path. It produces recommendations for the X32 matrix EQ and
// never applies anything: sending processed audio back into the PA would risk
// latency and feedback, and an automatic room EQ is the operator's decision.
class RoomCalibration final : private juce::Thread
{
public:
    RoomCalibration();
    ~RoomCalibration() override;

    bool startAnalysis(const juce::File& measurement);
    void stop();
    [[nodiscard]] RoomResult getResult() const;

    // Deterministic entry point, also used by the tests.
    static RoomResult analyse(const float* mono, int sampleCount, double sampleRate);

private:
    void run() override;
    void writeReport(const juce::File& target, const RoomResult& value) const;
    void setError(const juce::String& message);

    mutable juce::CriticalSection resultLock;
    RoomResult result;
    juce::File sourceFile;
};
} // namespace churchstream
