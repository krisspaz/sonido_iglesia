#include "OBSLocator.h"

#include <array>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <tlhelp32.h>
#endif

namespace churchstream
{
juce::File OBSLocator::findExecutable()
{
#if JUCE_WINDOWS
    const auto programFiles = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);
    const auto candidates = std::array<juce::File, 4> {
        programFiles.getChildFile("obs-studio\\bin\\64bit\\obs64.exe"),
        juce::File(juce::SystemStats::getEnvironmentVariable("ProgramFiles", {}))
            .getChildFile("obs-studio\\bin\\64bit\\obs64.exe"),
        juce::File(juce::SystemStats::getEnvironmentVariable("ProgramFiles(x86)", {}))
            .getChildFile("obs-studio\\bin\\64bit\\obs64.exe"),
        juce::File(juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {}))
            .getChildFile("Programs\\obs-studio\\bin\\64bit\\obs64.exe")
    };
    for (const auto& candidate : candidates)
        if (candidate.existsAsFile()) return candidate;
#elif JUCE_MAC
    const juce::File candidate("/Applications/OBS.app/Contents/MacOS/OBS");
    if (candidate.existsAsFile()) return candidate;
#endif
    return {};
}

bool OBSLocator::isRunning()
{
#if JUCE_WINDOWS
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    auto found = false;
    if (Process32FirstW(snapshot, &entry) != FALSE)
        do
        {
            if (juce::String(entry.szExeFile).equalsIgnoreCase("obs64.exe")) { found = true; break; }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    CloseHandle(snapshot);
    return found;
#else
    return false;
#endif
}

bool OBSLocator::openOBS()
{
    const auto executable = findExecutable();
    return executable.existsAsFile() && executable.startAsProcess();
}
} // namespace churchstream
