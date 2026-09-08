#include "QualityPanel.h"

namespace churchstream
{
QualityPanel::QualityPanel()
{
    setInterceptsMouseClicks(false, false);
}

void QualityPanel::setScores(const QualityScores& newScores)
{
    scores = newScores;
    overall = newScores.overall;
    overallText = juce::String(static_cast<int>(std::round(overall))) + " / 100";
    hasScores = newScores.overall > 0.0f;
    repaint();
}

std::array<QualityPanel::Dimension, 9> QualityPanel::buildDimensions() const noexcept
{
    return {{
        { "TONE", scores.tonalBalance },
        { "DYNAMICS", scores.dynamics },
        { "LOUDNESS", scores.loudness },
        { "TRUE PEAK", scores.truePeak },
        { "CLARITY", scores.clarity },
        { "STEREO", scores.stereo },
        { "NOISE", scores.noise },
        { "COMPRESS", scores.compression },
        { "STABILITY", scores.stability },
    }};
}

static juce::Colour scoreColour(float value)
{
    if (value >= 86.0f) return Colours::primary;
    if (value >= 70.0f) return Colours::warning;
    return Colours::danger;
}

void QualityPanel::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();
    if (!hasScores)
    {
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
        graphics.drawText("CALIBRATING - VERIFY INPUT SIGNAL", bounds, juce::Justification::centred);
        return;
    }

    // Overall bar on top.
    auto overallLabel = bounds.removeFromTop(16.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(10.0f).withStyle("Bold")));
    graphics.drawText("OVERALL", overallLabel.removeFromLeft(90.0f), juce::Justification::centredLeft);
    graphics.setColour(Colours::text);
    graphics.setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Bold")));
    graphics.drawText(overallText, overallLabel, juce::Justification::centredRight);

    auto barBounds = bounds.removeFromTop(14.0f).reduced(0.0f, 3.0f);
    graphics.setColour(Colours::control);
    graphics.fillRoundedRectangle(barBounds, 3.0f);
    const auto fillWidth = barBounds.getWidth() * juce::jlimit(0.0f, 1.0f, overall / 100.0f);
    if (fillWidth > 0.0f)
    {
        graphics.setColour(scoreColour(overall));
        graphics.fillRoundedRectangle(barBounds.withWidth(fillWidth), 3.0f);
    }

    bounds.removeFromTop(10.0f);

    // Per-dimension rows.
    const auto rowHeight = bounds.getHeight() / 9.0f;
    const auto labelWidth = bounds.getWidth() * 0.36f;
    const auto valueWidth = 44.0f;
    const auto barWidth = bounds.getWidth() - labelWidth - valueWidth - 8.0f;
    for (const auto& dim : buildDimensions())
    {
        auto row = bounds.removeFromTop(rowHeight);
        auto label = row.removeFromLeft(labelWidth);
        auto value = row.removeFromRight(valueWidth);
        auto bar = row.removeFromLeft(barWidth);

        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(8.5f).withStyle("Bold")));
        graphics.drawText(dim.name, label, juce::Justification::centredLeft);

        auto track = bar.reduced(0.0f, 2.0f);
        graphics.setColour(Colours::control);
        graphics.fillRoundedRectangle(track, 2.0f);
        const auto dimFill = track.getWidth() * juce::jlimit(0.0f, 1.0f, dim.value / 100.0f);
        if (dimFill > 0.0f)
        {
            graphics.setColour(scoreColour(dim.value));
            graphics.fillRoundedRectangle(track.withWidth(dimFill), 2.0f);
        }

        graphics.setColour(Colours::text);
        graphics.setFont(juce::Font(juce::FontOptions(8.5f).withStyle("Regular")));
        graphics.drawText(juce::String(static_cast<int>(std::round(dim.value))), value,
                          juce::Justification::centredRight);
    }
}
} // namespace churchstream
