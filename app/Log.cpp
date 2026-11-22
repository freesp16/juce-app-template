#include "app/Log.h"

#include "ProjectIdentity.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// The program's text segment (linker-defined): the stack scan keeps the words that point into it.
extern "C" char __executable_start, etext;

namespace app
{
void logLine(const char* fmt, ...)
{
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s %s\n", identity::envPrefix, buf);
    std::fflush(stdout);
}

void nameThisThread(const char* name) { prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0); }

// ---- the crash report (freesp16's app/Diagnostics.cpp, plus a stack scan). Async-signal-safe: write() only. ----
namespace
{
alignas(16) char gAltStack[64 * 1024];

void put(const char* s) noexcept
{
    std::size_t n = std::strlen(s);
    while (n > 0)
    {
        const ssize_t w = ::write(STDOUT_FILENO, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= static_cast<std::size_t>(w);
    }
}

void putHex(std::uintptr_t v) noexcept
{
    char buf[2 + 2 * sizeof v + 1];
    buf[0] = '0';
    buf[1] = 'x';
    for (std::size_t i = 0; i < 2 * sizeof v; ++i)
        buf[2 + i] = "0123456789abcdef"[(v >> (4 * (2 * sizeof v - 1 - i))) & 15];
    buf[sizeof buf - 1] = 0;
    put(buf);
}

void putDec(long v) noexcept
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    const bool neg = v < 0;
    unsigned long u = neg ? static_cast<unsigned long>(-(v + 1)) + 1 : static_cast<unsigned long>(v);
    do
        buf[--i] = static_cast<char>('0' + u % 10);
    while ((u /= 10) != 0 && i > 1);
    if (neg)
        buf[--i] = '-';
    put(buf + i);
}

void putTag(const char* what) noexcept
{
    put(identity::envPrefix);
    put(" CRASH ");
    put(what);
}

// The r-xp file mappings (an address in a library minus its mapping start + offset is what addr2line wants; the
// non-PIE program sits at its link address).
void putExecutableMaps() noexcept
{
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[4096], line[512];
    std::size_t len = 0;
    for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;)
        for (ssize_t i = 0; i < n; ++i)
        {
            if (buf[i] != '\n')
            {
                if (len + 1 < sizeof line)
                    line[len++] = buf[i];
                continue;
            }
            line[len] = 0;
            const char* perms = std::strchr(line, ' ');
            if (perms != nullptr && perms[3] == 'x' && std::strchr(line, '/') != nullptr)
            {
                putTag("map ");
                put(line);
                put("\n");
            }
            len = 0;
        }
    ::close(fd);
}

// Words on the crashed thread's stack that point into the program's text: return addresses, innermost first. ARM
// code without frame pointers cannot be unwound through the signal frame, so this is the call chain's best evidence.
void putStackScan(std::uintptr_t sp) noexcept
{
    const auto lo = reinterpret_cast<std::uintptr_t>(&__executable_start), hi = reinterpret_cast<std::uintptr_t>(&etext);
    putTag("stack");
    int found = 0;
    const auto* p = reinterpret_cast<const std::uintptr_t*>(sp & ~static_cast<std::uintptr_t>(sizeof(void*) - 1));
    for (int i = 0; i < 1024 && found < 40; ++i) // 4 KB (ARM) / 8 KB of stack; it is mapped while the thread runs
    {
        const std::uintptr_t v = p[i];
        if (v > lo && v < hi)
        {
            put(" ");
            putHex(v & ~static_cast<std::uintptr_t>(1)); // Thumb bit off
            ++found;
        }
    }
    put("\n");
}

void onFatalSignal(int sig, siginfo_t* info, void* context) noexcept
{
    putTag(sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : "SIGABRT");
    put(" addr=");
    putHex(info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
    std::uintptr_t sp = 0;
#if defined(__arm__)
    if (context != nullptr)
    {
        const auto* uc = static_cast<const ucontext_t*>(context);
        put(" pc=");
        putHex(uc->uc_mcontext.arm_pc);
        put(" lr=");
        putHex(uc->uc_mcontext.arm_lr);
        put(" sp=");
        putHex(sp = uc->uc_mcontext.arm_sp);
    }
#elif defined(__x86_64__)
    if (context != nullptr)
    {
        const auto* uc = static_cast<const ucontext_t*>(context);
        put(" pc=");
        putHex(static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
        put(" sp=");
        putHex(sp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]));
    }
#endif
    char name[17] = {};
    prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0);
    put(" thread=");
    put(name);
    put(" tid=");
    putDec(static_cast<long>(::syscall(SYS_gettid)));
    put(" pid=");
    putDec(static_cast<long>(::getpid()));
    put("\n");
    putExecutableMaps();
    if (sp != 0) // last: reading past the top of a small thread stack could fault again
        putStackScan(sp);
    putTag("end (resolve: addr2line -C -f -i -e <the unstripped build/armel/bin/<name>> <pc> <lr> <stack…>)\n");
    ::fsync(STDOUT_FILENO);
    ::signal(sig, SIG_DFL); // die with the default action: the exit status names the signal
    ::raise(sig);
}
} // namespace

void installCrashLog()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    stack_t ss {};
    ss.ss_sp = gAltStack;
    ss.ss_size = sizeof gAltStack;
    ::sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = &onFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        ::sigaction(sig, &sa, nullptr);
}
} // namespace app
