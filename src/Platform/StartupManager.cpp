#include "StartupManager.h"

namespace churchstream
{
namespace
{
const juce::String registryPath = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\ChurchStreamProcessor";
}

bool StartupManager::isEnabled()
{
#if JUCE_WINDOWS
    return juce::WindowsRegistry::valueExists(registryPath);
#else
    return false;
#endif
}

bool StartupManager::setEnabled(bool enabled, bool startMinimized)
{
#if JUCE_WINDOWS
    if (!enabled) return juce::WindowsRegistry::deleteValue(registryPath);
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    const auto command = "\"" + executable + "\"" + (startMinimized ? " --minimized" : "");
    return juce::WindowsRegistry::setValue(registryPath, command);
#else
    juce::ignoreUnused(enabled, startMinimized);
    return false;
#endif
}
} // namespace churchstream

