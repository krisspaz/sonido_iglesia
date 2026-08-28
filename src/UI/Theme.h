#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
namespace Colours
{
inline const juce::Colour background { 0xff0a0d12 };
inline const juce::Colour card { 0xff121821 };
inline const juce::Colour cardBorder { 0xff232c39 };
inline const juce::Colour primary { 0xff5ce1a5 };
inline const juce::Colour cyan { 0xff50c9ff };
inline const juce::Colour warning { 0xffffc857 };
inline const juce::Colour danger { 0xffff5c70 };
inline const juce::Colour text { 0xfff4f7fa };
inline const juce::Colour mutedText { 0xff8e9aaa };
inline const juce::Colour control { 0xff1a222d };
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

