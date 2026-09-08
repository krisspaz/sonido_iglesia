#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
namespace Colours
{
inline const juce::Colour background { 0xff0f1117 };
inline const juce::Colour card { 0xff1a1d27 };
inline const juce::Colour cardBorder { 0xff2d3348 };
inline const juce::Colour primary { 0xff6366f1 };
inline const juce::Colour cyan { 0xff818cf8 };
inline const juce::Colour warning { 0xfffbbf24 };
inline const juce::Colour danger { 0xfff43f5e };
inline const juce::Colour text { 0xffe2e8f0 };
inline const juce::Colour mutedText { 0xff64748b };
inline const juce::Colour control { 0xff242938 };
} // namespace Colours

class Theme final : public juce::LookAndFeel_V4
{
public:
    Theme();

    void drawButtonBackground(juce::Graphics&,
                              juce::Button&,
                              const juce::Colour&,
                              bool isMouseOverButton,
                              bool isButtonDown) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
};
} // namespace churchstream

