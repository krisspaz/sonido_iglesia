#include "SystemMonitor.h"

#include <algorithm>
#include <sstream>
#include <string>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#elif JUCE_MAC
 #include <mach/mach.h>
 #include <mach/mach_host.h>
 #include <mach/task_info.h>
 #include <sys/resource.h>
#elif JUCE_LINUX
 #include <sys/resource.h>
 #include <unistd.h>
 #include <fstream>
#endif

namespace churchstream
{
namespace
{
#if JUCE_WINDOWS
uint64_t fileTimeToInteger(const FILETIME& value)
{
    ULARGE_INTEGER integer;
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}
#endif
}

double SystemMonitor::sampleSystemCpuPercent()
{
    uint64_t idle = 0;
    uint64_t total = 0;

#if JUCE_WINDOWS
    FILETIME idleTime {}, kernelTime {}, userTime {};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime) == 0)
        return 0.0;
    idle = fileTimeToInteger(idleTime);
    total = fileTimeToInteger(kernelTime) + fileTimeToInteger(userTime);
#elif JUCE_MAC
    host_cpu_load_info_data_t info {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&info), &count) != KERN_SUCCESS)
        return 0.0;
    idle = info.cpu_ticks[CPU_STATE_IDLE];
    total = info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM]
        + info.cpu_ticks[CPU_STATE_IDLE] + info.cpu_ticks[CPU_STATE_NICE];
#elif JUCE_LINUX
    std::ifstream stat("/proc/stat");
    juce::String cpu;
    uint64_t user = 0, nice = 0, system = 0, idleTicks = 0, ioWait = 0, irq = 0, softIrq = 0;
    stat >> cpu >> user >> nice >> system >> idleTicks >> ioWait >> irq >> softIrq;
    idle = idleTicks + ioWait;
    total = user + nice + system + idleTicks + ioWait + irq + softIrq;
#else
    return 0.0;
#endif

    if (previousTotal == 0 || total <= previousTotal)
    {
        previousIdle = idle;
        previousTotal = total;
        return 0.0;
    }

    const auto totalDelta = total - previousTotal;
    const auto idleDelta = idle >= previousIdle ? idle - previousIdle : 0;
    previousIdle = idle;
    previousTotal = total;
    return 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
}

double SystemMonitor::sampleProcessCpuPercent()
{
    uint64_t processTimeNanoseconds = 0;

#if JUCE_WINDOWS
    FILETIME creation {}, exit {}, kernel {}, user {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
        return 0.0;
    processTimeNanoseconds = (fileTimeToInteger(kernel) + fileTimeToInteger(user)) * 100ULL;
#elif JUCE_MAC || JUCE_LINUX
    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    processTimeNanoseconds = static_cast<uint64_t>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
            * 1000000000ULL
        + static_cast<uint64_t>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ULL;
#else
    return 0.0;
#endif

    const auto wallTimeNanoseconds = juce::Time::getMillisecondCounterHiRes() * 1000000.0;
    if (previousProcessTime == 0 || previousProcessWallTime <= 0.0
        || processTimeNanoseconds < previousProcessTime
        || wallTimeNanoseconds <= previousProcessWallTime)
    {
        previousProcessTime = processTimeNanoseconds;
        previousProcessWallTime = wallTimeNanoseconds;
        return 0.0;
    }

    const auto processDelta = static_cast<double>(processTimeNanoseconds - previousProcessTime);
    const auto wallDelta = wallTimeNanoseconds - previousProcessWallTime;
    previousProcessTime = processTimeNanoseconds;
    previousProcessWallTime = wallTimeNanoseconds;
    const auto processors = static_cast<double>(std::max(1, juce::SystemStats::getNumCpus()));
    return std::clamp(100.0 * processDelta / wallDelta / processors, 0.0, 100.0);
}

double SystemMonitor::getProcessMemoryMegabytes()
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != 0)
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif JUCE_MAC
    mach_task_basic_info_data_t info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#elif JUCE_LINUX
    std::ifstream stat("/proc/self/statm");
    long pages = 0;
    long residentPages = 0;
    stat >> pages >> residentPages;
    if (residentPages > 0)
        return static_cast<double>(residentPages * sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
    return 0.0;
}
} // namespace churchstream
