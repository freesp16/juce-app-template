// One log line per event, on stdout: "<PREFIX> <TAG> …" (on the unit the boot menu writes the app's output into
// bootmenu.log on the stick; stdout is line-buffered, so a crash never loses the lines before it).
#pragma once

#include <chrono>
#include <cstdio>

namespace app
{
void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

inline double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Line-buffered stdout, and a fatal signal writes "CRASH …" lines before the process dies: the faulting address, pc, lr
// and sp, the thread's name, the executable mappings and the return addresses found on the stack.
void installCrashLog();
// The calling thread's name (15 characters), as the crash report and `top -H` show it.
void nameThisThread(const char* name);
} // namespace app
