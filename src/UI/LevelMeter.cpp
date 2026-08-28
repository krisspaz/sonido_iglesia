#include "LevelMeter.h"

namespace churchstream
{
LevelMeter::LevelMeter(juce::String meterName)
    : name(std::move(meterName))
{
    setInterceptsMouseClicks(false, false);
}

void LevelMeter::setLevels(float leftPeak, float rightPeak, float leftRms, float rightRms)
{
    const std::array<float, 2> newPeaks { leftPeak, rightPeak };
    const std::array<float, 2> newRms { leftRms, rightRms };

    for (size_t channel = 0; channel < displayedPeak.size(); ++channel)
    {
        displayedPeak[channel] = juce::jmax(newPeaks[channel], displayedPeak[channel] * 0.86f);
        displayedRms[channel] = juce::jmax(newRms[channel], displayedRms[channel] * 0.91f);
    }

    repaint();
}

void LevelMeter::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
    graphics.drawText(name.toUpperCase(), bounds.removeFromTop(20.0f), juce::Justification::centredLeft);

    bounds.removeFromTop(4.0f);
    const auto channelHeight = (bounds.getHeight() - 8.0f) * 0.5f;
    drawChannel(graphics, bounds.removeFromTop(channelHeight), 0, "L");
    bounds.removeFromTop(8.0f);
    drawChannel(graphics, bounds.removeFromTop(channelHeight), 1, "R");
}

float LevelMeter::gainToPosition(float gain) noexcept
{
    const auto decibels = juce::Decibels::gainToDecibels(gain, -60.0f);
    return juce::jlimit(0.0f, 1.0f, (decibels + 60.0f) / 60.0f);
}

void LevelMeter::drawChannel(juce::Graphics& graphics, juce::Rectangle<float> bounds,
                             int channel, const juce::String& channelName)
{
    auto labelBounds = bounds.removeFromLeft(20.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
    graphics.drawText(channelName, labelBounds, juce::Justification::centredLeft);

    auto valueBounds = bounds.removeFromRight(58.0f);
    bounds = bounds.reduced(0.0f, 4.0f);
    graphics.setColour(Colours::background.brighter(0.08f));
    graphics.fillRoundedRectangle(bounds, 4.0f);

    const auto rmsWidth = bounds.getWidth() * gainToPosition(displayedRms[static_cast<size_t>(channel)]);
    auto rmsBounds = bounds.withWidth(rmsWidth);
    juce::ColourGradient gradient(Colours::cyan.darker(0.12f), bounds.getX(), bounds.getY(),
                                  Colours::primary, bounds.getRight(), bounds.getY(), false);
    gradient.addColour(0.88, Colours::warning);
    gradient.addColour(0.97, Colours::danger);
    graphics.setGradientFill(gradient);
    graphics.fillRoundedRectangle(rmsBounds, 4.0f);

    const auto peakX = bounds.getX() + bounds.getWidth()
        * gainToPosition(displayedPeak[static_cast<size_t>(channel)]);
    graphics.setColour(displayedPeak[static_cast<size_t>(channel)] >= 1.0f ? Colours::danger : Colours::text);
    graphics.fillRect(juce::Rectangle<float>(juce::jlimit(bounds.getX(), bounds.getRight() - 2.0f, peakX),
                                             bounds.getY(), 2.0f, bounds.getHeight()));

    const auto decibels = juce::Decibels::gainToDecibels(displayedPeak[static_cast<size_t>(channel)], -60.0f);
    graphics.setColour(decibels > -1.0f ? Colours::danger : Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Regular")));
    graphics.drawText(juce::String(decibels, 1) + " dB", valueBounds,
                      juce::Justification::centredRight);
}
} // namespace churchstream

