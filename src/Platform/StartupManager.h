#pragma once

#include <juce_core/juce_core.h>

namespace churchstream
{
struct StartupManager final
{
    [[nodiscard]] static bool isEnabled();
    static bool setEnabled(bool enabled, bool startMinimized);
};
} // namespace churchstream

