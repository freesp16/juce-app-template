// The touch strip source (see StripSource.h). Device only.
#include "imx6/StripSource.h"

#include "imx6/DriverIoctl.h"
#include "imx6/InputTrace.h"

#include "Platform.h"
#include "ProjectIdentity.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace platform::imx6
{
namespace
{
constexpr const char* stripPath = "/dev/cdc_spi2.0";
constexpr unsigned long reqReset = 0x40017006UL, reqSetInterval = 0x40047002UL, reqSetBytes = 0x40047004UL;

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

StripSource::~StripSource() { stop(); }

bool StripSource::start(std::string& info)
{
    fd_ = ::open(stripPath, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
    {
        info = std::string("cdc-strip: cannot open ") + stripPath + ": " + std::strerror(errno);
        return false;
    }
    // Timer off first: the driver refuses bytes while it runs, and the stock app leaves it on (DriverIoctl.h).
    std::uint8_t reset = 1, timerOff = 0, timerOn = 1;
    std::uint32_t bytes = strip::kFrameBytes, interval = 3;
    if (! driverIoctl(fd_, "cdc-strip", "timer off", reqSetTimer, &timerOff, info)
        || ! driverIoctl(fd_, "cdc-strip", "strip reset", reqReset, &reset, info)
        || ! driverIoctl(fd_, "cdc-strip", "set bytes", reqSetBytes, &bytes, info)
        || ! driverIoctl(fd_, "cdc-strip", "set interval", reqSetInterval, &interval, info)
        || ! driverIoctl(fd_, "cdc-strip", "timer on", reqSetTimer, &timerOn, info))
    {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    info = std::string("cdc-strip: 4-byte frames from ") + stripPath;
    return true;
}

void StripSource::stop()
{
    quit_ = true;
    const bool ran = thread_.joinable();
    if (ran)
        thread_.join();
    if (fd_ >= 0)
    {
        driverTimerOff(fd_); // leave the device idle for the stock app
        ::close(fd_);
    }
    fd_ = -1;
    if (ran) // the reader's totals (message thread, after the join)
    {
        std::printf("[sp16-mod] %s CDC-STRIP frames=%ld events=%ld dropped=%ld engine_dropped=%ld\n",
                    identity::envPrefix, frames_.load(), events_.load(), dropped_.load(), engineDropped_.load());
        std::fflush(stdout);
    }
}

void StripSource::run()
{
    std::uint8_t frame[strip::kFrameBytes];
    while (! quit_)
    {
        pollfd p {fd_, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0)
            continue;
        const auto n = ::read(fd_, frame, sizeof frame);
        if (n != strip::kFrameBytes)
        {
            if (n < 0 && errno != EAGAIN && errno != EINTR)
                ::usleep(10000);
            continue;
        }
        frames_.fetch_add(1, std::memory_order_relaxed);
        io::ControlEvent e;
        if (! decoder_.decode(frame, nowMs(), e))
            continue;
        events_.fetch_add(1, std::memory_order_relaxed);
        // The app's TriggerSink first (the latency path): Touch / Value = contact at the position, Release = lifted.
        if (engine_ != nullptr
            && ! engine_->push(platform::stripTrigger(e.value, e.kind != io::ControlEvent::Kind::Release)))
            engineDropped_.fetch_add(1, std::memory_order_relaxed);
        if (! queue_.push(e))
            dropped_.fetch_add(1, std::memory_order_relaxed);
        if (lamps_)
            lamps_->echo(e);
    }
}

void StripSource::poll(std::vector<io::ControlEvent>& out)
{
    io::ControlEvent e;
    while (queue_.pop(e))
    {
        if (trace_)
            traceInput(id(), e);
        out.push_back(e);
    }
}
} // namespace platform::imx6
