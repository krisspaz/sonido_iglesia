#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>

namespace churchstream
{
// Offline-only bridge. It never runs on the audio callback and never owns a
// DSP object. Failure is reported to the UI and is otherwise ignored.
class ChurchSoundAnalystClient final
{
public:
    using Completion = std::function<void(bool, juce::String)>;

    explicit ChurchSoundAnalystClient(juce::String token);
    ~ChurchSoundAnalystClient();

    void analyzeFile(const juce::File& file, Completion completion);
    void request(const juce::String& endpoint, juce::var payload, Completion completion);
    [[nodiscard]] const juce::String& getToken() const noexcept { return token; }

private:
    void startBundledService();
    class Job;
    juce::ThreadPool pool { 1 };
    juce::ChildProcess bundledService;
    juce::String token;
};
} // namespace churchstream
