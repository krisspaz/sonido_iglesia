#pragma once

#include "Analysis/AnalysisTypes.h"
#include "Theme.h"

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
class SpectrumComponent final : public juce::Component
{
public:
    void setSnapshot(const AnalysisSnapshot& snapshot);
    void paint(juce::Graphics&) override;

private:
    static float frequencyToX(float frequency, juce::Rectangle<float> bounds) noexcept;
    static float decibelsToY(float decibels, juce::Rectangle<float> bounds) noexcept;
    static juce::Path createPath(const std::array<float, spectrumBins>& values,
                                 double sampleRate, juce::Rectangle<float> bounds);

    std::array<float, spectrumBins> input {};
    std::array<float, spectrumBins> processed {};
    double sampleRate = 48000.0;
    bool hasData = false;
};
} // namespace churchstream

