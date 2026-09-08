#include "TrayController.h"

#include "Theme.h"

namespace churchstream
{
TrayController::TrayController()
{
    const auto icon = createIcon();
    setIconImage(icon, icon);
    setIconTooltip("Church Stream Processor");
}

void TrayController::mouseDown(const juce::MouseEvent&)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Open");
    menu.addSeparator();
    menu.addItem(2, "Bypass");
    menu.addItem(3, "Resume Processing");
    menu.addItem(4, "Connect OBS");
    menu.addSeparator();
    menu.addItem(5, "Quit");
    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result)
    {
        if (result == 1 && open) open();
        else if (result == 2 && bypass) bypass();
        else if (result == 3 && resume) resume();
        else if (result == 4 && connectObs) connectObs();
        else if (result == 5 && quit) quit();
    });
}

juce::Image TrayController::createIcon()
{
    juce::Image image(juce::Image::ARGB, 32, 32, true);
    juce::Graphics graphics(image);
    graphics.setColour(Colours::background);
    graphics.fillRoundedRectangle(image.getBounds().toFloat(), 7.0f);
    graphics.setColour(Colours::primary);
    graphics.fillEllipse(5.0f, 5.0f, 22.0f, 22.0f);
    graphics.setColour(Colours::background);
    graphics.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
    graphics.drawText("C", image.getBounds(), juce::Justification::centred);
    return image;
}
} // namespace churchstream

