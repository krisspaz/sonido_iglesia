#pragma once

#include <juce_core/juce_core.h>

namespace churchstream
{
struct WindowsAudioEndpoint final
{
    [[nodiscard]] static juce::String findCaptureEndpointId(const juce::String& friendlyName);
};
} // namespace churchstream

