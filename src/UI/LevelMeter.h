#pragma once

#include "Theme.h"

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
class LevelMeter final : public juce::Component
{
public:
    explicit LevelMeter(juce::String meterName);

    void setLevels(float leftPeak, float rightPeak, float leftRms, float rightRms);
    void paint(juce::Graphics&) override;

private:
    static float gainToPosition(float gain) noexcept;
    void drawChannel(juce::Graphics&, juce::Rectangle<float> bounds,
                     int channel, const juce::String& channelName);

    juce::String name;
    std::array<float, 2> displayedPeak { 0.0f, 0.0f };
    std::array<float, 2> displayedRms { 0.0f, 0.0f };
};
} // namespace churchstream
