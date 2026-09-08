#pragma once

#include "Smart/SmartTypes.h"
#include "Theme.h"

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
// A compact bar + per-dimension breakdown of the Smart Engine quality score.
// The overall number alone cannot tell an operator what is wrong; this panel
// turns each 0-100 dimension into a colour-coded bar so the weakest link is
// visible at a glance.
class QualityPanel final : public juce::Component
{
public:
    QualityPanel();

    void setScores(const QualityScores& scores);
    void paint(juce::Graphics&) override;

private:
    struct Dimension
    {
        const char* name;
        float value; // 0-100
    };

    [[nodiscard]] std::array<Dimension, 9> buildDimensions() const noexcept;

    QualityScores scores;
    juce::String overallText { "--" };
    float overall = 0.0f;
    bool hasScores = false;
};
} // namespace churchstream
