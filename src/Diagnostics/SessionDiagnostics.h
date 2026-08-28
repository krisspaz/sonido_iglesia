#pragma once

#include "Analysis/AnalysisTypes.h"
#include "Safety/SafetyController.h"
#include "Smart/SmartTypes.h"

#include <map>
#include <juce_core/juce_core.h>

namespace churchstream
{
class SessionDiagnostics final
{
public:
    explicit SessionDiagnostics(const juce::File& dataDirectory);
    ~SessionDiagnostics();

    void observe(const AnalysisSnapshot& analysis, const SmartState& smart,
                 const SafetyState& safety, double processCpuPercent, bool streamActive);
    void finish();
    [[nodiscard]] juce::File getReportFile() const { return reportFile; }

private:
    void addEvent(const juce::String& type, const juce::String& detail,
                  float confidence = 0.0f, float changeDb = 0.0f);
    void writeReport(bool complete);

    juce::File reportFile;
    juce::Time startTime { juce::Time::getCurrentTime() };
    double lastObservationMs = 0.0;
    double lastWriteMs = 0.0;
    double activeSeconds = 0.0;
    double loudnessSum = 0.0;
    double cpuSum = 0.0;
    int loudnessSamples = 0;
    int cpuSamples = 0;
    float maximumTruePeak = -100.0f;
    float scoreSum = 0.0f;
    int scoreSamples = 0;
    int rollbackCount = 0;
    std::map<std::string, int> correctionCounts;
    std::map<std::string, juce::String> lastActionResults;
    std::map<std::string, bool> activeSafetyEvents;
    juce::Array<juce::var> events;
    bool finished = false;
};
} // namespace churchstream
