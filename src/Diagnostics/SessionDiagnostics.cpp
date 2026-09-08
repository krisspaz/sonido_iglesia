#include "SessionDiagnostics.h"

#include <algorithm>

namespace churchstream
{
SessionDiagnostics::SessionDiagnostics(const juce::File& dataDirectory)
{
    const auto directory = dataDirectory.getChildFile("Session Reports");
    directory.createDirectory();
    reportFile = directory.getChildFile("session-" + startTime.formatted("%Y%m%d-%H%M%S") + ".json");
}

SessionDiagnostics::~SessionDiagnostics() { finish(); }

void SessionDiagnostics::observe(const AnalysisSnapshot& analysis, const SmartState& smart,
                                 const SafetyState& safety, double processCpuPercent, bool streamActive)
{
    if (finished) return;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto elapsed = lastObservationMs > 0.0 ? std::clamp((now - lastObservationMs) * 0.001, 0.0, 5.0) : 0.0;
    lastObservationMs = now;
    if (streamActive) activeSeconds += elapsed;

    if (analysis.processed.lufsShortTerm > -70.0f)
    {
        loudnessSum += analysis.processed.lufsShortTerm;
        ++loudnessSamples;
    }
    maximumTruePeak = std::max(maximumTruePeak, analysis.processed.truePeakDbtp);
    cpuSum += processCpuPercent;
    ++cpuSamples;
    if (smart.quality.overall > 0.0f)
    {
        scoreSum += smart.quality.overall;
        ++scoreSamples;
    }

    std::map<std::string, bool> currentActions;
    for (int index = 0; index < smart.actionCount; ++index)
    {
        const auto& action = smart.actions[static_cast<size_t>(index)];
        const auto key = action.name.toStdString();
        currentActions[key] = true;
        const auto previous = lastActionResults.find(key);
        const auto changed = previous == lastActionResults.end() || previous->second != action.result;
        if (changed)
        {
            ++correctionCounts[key];
            addEvent(action.rolledBack ? "rollback" : "correction", action.name + " | " + action.result,
                     action.confidence, action.amountDb);
            if (action.rolledBack) ++rollbackCount;
            lastActionResults[key] = action.result;
        }
    }

    std::map<std::string, bool> currentSafety;
    for (int index = 0; index < safety.eventCount; ++index)
    {
        const auto& event = safety.events[static_cast<size_t>(index)];
        const auto key = event.name.toStdString();
        currentSafety[key] = true;
        if (!activeSafetyEvents[key])
            addEvent("safety", event.name + " | " + event.response);
    }
    activeSafetyEvents = std::move(currentSafety);

    if (lastWriteMs <= 0.0 || now - lastWriteMs >= 30000.0)
    {
        lastWriteMs = now;
        writeReport(false);
    }
}

void SessionDiagnostics::finish()
{
    if (finished) return;
    finished = true;
    writeReport(true);
}

void SessionDiagnostics::addEvent(const juce::String& type, const juce::String& detail,
                                  float confidence, float changeDb)
{
    if (events.size() >= 5000) return;
    auto event = juce::DynamicObject::Ptr(new juce::DynamicObject());
    event->setProperty("time", juce::Time::getCurrentTime().toISO8601(true));
    event->setProperty("type", type);
    event->setProperty("detail", detail);
    event->setProperty("confidence", confidence);
    event->setProperty("changeDb", changeDb);
    events.add(juce::var(event.get()));
}

void SessionDiagnostics::writeReport(bool complete)
{
    auto root = juce::DynamicObject::Ptr(new juce::DynamicObject());
    root->setProperty("schemaVersion", 1);
    root->setProperty("started", startTime.toISO8601(true));
    root->setProperty("updated", juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("complete", complete);
    root->setProperty("streamDurationSeconds", activeSeconds);
    root->setProperty("averageLoudnessLufs", loudnessSamples > 0 ? loudnessSum / loudnessSamples : -100.0);
    root->setProperty("maximumTruePeakDbtp", maximumTruePeak);
    root->setProperty("averageCpuPercent", cpuSamples > 0 ? cpuSum / cpuSamples : 0.0);
    root->setProperty("overallMixScore", scoreSamples > 0
        ? scoreSum / static_cast<float>(scoreSamples) : 0.0f);
    root->setProperty("rollbacks", rollbackCount);
    juce::Array<juce::var> corrections;
    for (const auto& [name, count] : correctionCounts)
    {
        auto correction = juce::DynamicObject::Ptr(new juce::DynamicObject());
        correction->setProperty("name", juce::String(name));
        correction->setProperty("count", count);
        corrections.add(juce::var(correction.get()));
    }
    root->setProperty("corrections", juce::var(corrections));
    root->setProperty("events", juce::var(events));
    reportFile.replaceWithText(juce::JSON::toString(juce::var(root.get()), true));
}
} // namespace churchstream
