#include "Theme.h"

namespace churchstream
{
Theme::Theme()
{
    setColour(juce::Label::textColourId, Colours::text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, Colours::control);
    setColour(juce::ComboBox::textColourId, Colours::text);
    setColour(juce::ComboBox::outlineColourId, Colours::cardBorder);
    setColour(juce::ComboBox::arrowColourId, Colours::mutedText);
    setColour(juce::ComboBox::focusedOutlineColourId, Colours::primary.withAlpha(0.7f));
    setColour(juce::PopupMenu::backgroundColourId, Colours::card);
    setColour(juce::PopupMenu::textColourId, Colours::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, Colours::primary.withAlpha(0.18f));
    setColour(juce::PopupMenu::highlightedTextColourId, Colours::text);
    setColour(juce::TextButton::buttonColourId, Colours::primary);
    setColour(juce::TextButton::textColourOffId, Colours::background);
    setColour(juce::TooltipWindow::backgroundColourId, Colours::card);
    setColour(juce::TooltipWindow::textColourId, Colours::text);
    setColour(juce::TooltipWindow::outlineColourId, Colours::cardBorder);
}

void Theme::drawButtonBackground(juce::Graphics& graphics,
                                 juce::Button& button,
                                 const juce::Colour& backgroundColour,
                                 bool isMouseOverButton,
                                 bool isButtonDown)
{
    auto colour = backgroundColour;
    if (!button.isEnabled())
        colour = colour.withMultipliedAlpha(0.35f);
    else if (isButtonDown)
        colour = colour.darker(0.16f);
    else if (isMouseOverButton)
        colour = colour.brighter(0.10f);

    graphics.setColour(colour);
    graphics.fillRoundedRectangle(button.getLocalBounds().toFloat(), 9.0f);
}

void Theme::drawComboBox(juce::Graphics& graphics, int width, int height, bool isButtonDown,
                         int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                                static_cast<float>(width),
                                                static_cast<float>(height));
    graphics.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    graphics.fillRoundedRectangle(bounds, 9.0f);
    graphics.setColour(box.findColour(box.hasKeyboardFocus(false)
                                          ? juce::ComboBox::focusedOutlineColourId
                                          : juce::ComboBox::outlineColourId));
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 9.0f, 1.0f);

    const auto arrowArea = juce::Rectangle<float>(static_cast<float>(width - 34), 0.0f, 22.0f,
                                                   static_cast<float>(height));
    juce::Path arrow;
    const auto centre = arrowArea.getCentre();
    arrow.startNewSubPath(centre.x - 4.0f, centre.y - 2.0f);
    arrow.lineTo(centre.x, centre.y + 2.0f);
    arrow.lineTo(centre.x + 4.0f, centre.y - 2.0f);
    graphics.setColour(box.findColour(juce::ComboBox::arrowColourId)
                           .withMultipliedAlpha(isButtonDown ? 0.6f : 1.0f));
    graphics.strokePath(arrow, juce::PathStrokeType(1.8f,
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
}

juce::Font Theme::getComboBoxFont(juce::ComboBox&)
{
    return juce::Font(juce::FontOptions(14.0f).withStyle("Regular"));
}

juce::Font Theme::getTextButtonFont(juce::TextButton&, int)
{
    return juce::Font(juce::FontOptions(13.0f).withStyle("Bold"));
}
} // namespace churchstream

