#pragma once

#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>

namespace churchstream
{
class TrayController final : public juce::SystemTrayIconComponent
{
public:
    TrayController();
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void()> open;
    std::function<void()> bypass;
    std::function<void()> resume;
    std::function<void()> connectObs;
    std::function<void()> quit;

private:
    static juce::Image createIcon();
};
} // namespace churchstream

