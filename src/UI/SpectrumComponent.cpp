#include "SpectrumComponent.h"

#include <cmath>

namespace churchstream
{
void SpectrumComponent::setSnapshot(const AnalysisSnapshot& snapshot)
{
    input = snapshot.input.spectrumDb;
    processed = snapshot.processed.spectrumDb;
    sampleRate = snapshot.sampleRate > 0.0 ? snapshot.sampleRate : 48000.0;
    hasData = snapshot.analyzedFrames >= static_cast<uint64_t>(fftSize);
    repaint();
}

void SpectrumComponent::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    graphics.setColour(Colours::background.withAlpha(0.65f));
    graphics.fillRoundedRectangle(bounds, 10.0f);
    bounds = bounds.reduced(12.0f, 9.0f);

    graphics.setFont(juce::Font(juce::FontOptions(9.5f).withStyle("Regular")));
    for (const auto frequency : { 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
    {
        const auto x = frequencyToX(frequency, bounds);
        graphics.setColour(Colours::cardBorder.withAlpha(0.65f));
        graphics.drawVerticalLine(static_cast<int>(x), bounds.getY(), bounds.getBottom());
        graphics.setColour(Colours::mutedText.withAlpha(0.7f));
        const auto text = frequency >= 1000.0f ? juce::String(frequency / 1000.0f, frequency == 1000.0f ? 0 : 0) + "k"
                                              : juce::String(static_cast<int>(frequency));
        graphics.drawText(text, static_cast<int>(x - 15.0f), static_cast<int>(bounds.getBottom() - 13.0f),
                          30, 12, juce::Justification::centred);
    }
    for (const auto db : { -60.0f, -40.0f, -20.0f })
    {
        const auto y = decibelsToY(db, bounds);
        graphics.setColour(Colours::cardBorder.withAlpha(0.5f));
        graphics.drawHorizontalLine(static_cast<int>(y), bounds.getX(), bounds.getRight());
    }

    if (!hasData)
    {
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Regular")));
        graphics.drawText("Waiting for real audio...", bounds, juce::Justification::centred);
        return;
    }

    graphics.setColour(Colours::cyan.withAlpha(0.48f));
    graphics.strokePath(createPath(input, sampleRate, bounds),
                        juce::PathStrokeType(1.2f));
    graphics.setColour(Colours::primary);
    graphics.strokePath(createPath(processed, sampleRate, bounds),
                        juce::PathStrokeType(1.8f));

    graphics.setFont(juce::Font(juce::FontOptions(9.5f).withStyle("Bold")));
    graphics.setColour(Colours::cyan.withAlpha(0.7f));
    graphics.drawText("INPUT", bounds.removeFromTop(14.0f).removeFromRight(92.0f), juce::Justification::centredRight);
    graphics.setColour(Colours::primary);
    graphics.drawText("PROCESSED", bounds.removeFromTop(14.0f).removeFromRight(92.0f), juce::Justification::centredRight);
}

float SpectrumComponent::frequencyToX(float frequency, juce::Rectangle<float> bounds) noexcept
{
    const auto normalised = std::log10(std::clamp(frequency, 20.0f, 20000.0f) / 20.0f) / 3.0f;
    return bounds.getX() + normalised * bounds.getWidth();
}

float SpectrumComponent::decibelsToY(float decibels, juce::Rectangle<float> bounds) noexcept
{
    return bounds.getBottom() - std::clamp((decibels + 80.0f) / 80.0f, 0.0f, 1.0f) * bounds.getHeight();
}

juce::Path SpectrumComponent::createPath(const std::array<float, spectrumBins>& values,
                                         double rate, juce::Rectangle<float> bounds)
{
    juce::Path path;
    // The analyzer remains a real 2048-point FFT. Rendering every bin would
    // create >2k path segments per frame for no visible benefit, so sample it
    // on a logarithmic grid close to the display's useful horizontal detail.
    const auto points = std::clamp(static_cast<int>(bounds.getWidth() / 3.0f), 96, 384);
    for (int point = 0; point <= points; ++point)
    {
        const auto normalised = static_cast<float>(point) / static_cast<float>(points);
        const auto frequency = 20.0f * std::pow(1000.0f, normalised);
        const auto bin = std::clamp(static_cast<int>(std::round(frequency * fftSize / rate)),
                                    1, spectrumBins - 1);
        const auto x = frequencyToX(frequency, bounds);
        const auto y = decibelsToY(values[static_cast<size_t>(bin)], bounds);
        if (point == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }
    return path;
}
} // namespace churchstream
