#pragma once

#include <juce_core/juce_core.h>

namespace churchstream
{
struct OBSLocator final
{
    [[nodiscard]] static juce::File findExecutable();
    [[nodiscard]] static bool isRunning();
    static bool openOBS();
};
} // namespace churchstream

