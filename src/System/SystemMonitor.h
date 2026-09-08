#pragma once

#include <juce_core/juce_core.h>

namespace churchstream
{
class SystemMonitor final
{
public:
    // Call from the message/diagnostics thread only, never from the audio callback.
    double sampleSystemCpuPercent();
    double sampleProcessCpuPercent();
    [[nodiscard]] static double getProcessMemoryMegabytes();

private:
    uint64_t previousIdle = 0;
    uint64_t previousTotal = 0;
    uint64_t previousProcessTime = 0;
    double previousProcessWallTime = 0.0;
};
} // namespace churchstream
