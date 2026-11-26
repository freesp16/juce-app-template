// The hardware panel link (see PanelLink.h). Device only: open/ioctl/poll on /dev/subucom_spi0.1.
#include "imx6/PanelLink.h"

#include "imx6/DriverIoctl.h"
#include "imx6/InputTrace.h"


#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace platform::imx6
{
namespace
{
constexpr const char* panelPath = "/dev/subucom_spi0.1";
// Panel driver ioctl requests.
constexpr unsigned long reqSetInterval = 0x40047002UL, reqSetBits = 0x40027003UL, reqSetBytes = 0x40047004UL,
                        reqSetEndian = 0x40017009UL, reqTransfer = 0x40107000UL;
struct SpiTransfer // the driver's struct spi_message (16 bytes)
{
    std::uint8_t multiId, reserved, partId, partCount;
    std::uint32_t bytes;
    std::uint64_t txBuf;
};
static_assert(sizeof(SpiTransfer) == 16, "the SPI transfer descriptor is 16 bytes");

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

PanelLink::PanelLink(platform::Unit unit, const io::PadCalibration& pads, platform::TriggerSink* triggers,
                     std::shared_ptr<panel::LampBus> lamps, bool trace, std::shared_ptr<panel::AnalogBoard> board)
    : map_(panel::panelMap(unit)), triggers_(triggers), lamps_(std::move(lamps)), board_(std::move(board)), trace_(trace)
{
    panel::DecoderConfig config;
    config.unit = unit;
    config.pads = pads;
    decoder_.configure(config);
    decoder_.setTriggers(triggers_);
}

PanelLink::~PanelLink() { stop(); }

bool PanelLink::start(std::string& info)
{
    fd_ = ::open(panelPath, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
    {
        info = std::string("panel: cannot open ") + panelPath + ": " + std::strerror(errno);
        return false;
    }
    // Timer off first: the driver refuses bits/bytes while it runs, and the stock app leaves it on (DriverIoctl.h).
    // The DJS-1000's link is taken over running instead, when it is set up as wanted: a pause may mute its outputs.
    std::uint16_t bits = 32;
    std::uint8_t endian = 1, timerOff = 0, timerOn = 1;
    std::uint32_t bytes = panel::kFrameBytes, interval = map_.intervalMs;
    takenOver_ = map_.takeOver && driverRunningAs(fd_, bytes, interval);
    if (takenOver_)
        (void) ::ioctl(fd_, reqSetEndian, &endian);
    else if (! driverIoctl(fd_, "panel", "timer off", reqSetTimer, &timerOff, info)
        || ! driverIoctl(fd_, "panel", "set bits", reqSetBits, &bits, info)
        || ! driverIoctl(fd_, "panel", "set endian", reqSetEndian, &endian, info)
        || ! driverIoctl(fd_, "panel", "set bytes", reqSetBytes, &bytes, info)
        || ! driverIoctl(fd_, "panel", "set interval", reqSetInterval, &interval, info)
        || ! driverIoctl(fd_, "panel", "timer on", reqSetTimer, &timerOn, info))
    {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    int pipeFds[2];
    if (::pipe(pipeFds) == 0)
    {
        wakeRead_ = pipeFds[0];
        wakeWrite_ = pipeFds[1];
        for (int fd : pipeFds)
        {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    if (lamps_)
        lamps_->setWake(&PanelLink::wake, this);
    quit_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    info = std::string("panel: ") + std::string(platform::unitModel(map_.unit)) + ", 128-byte RX frames from " + panelPath
         + (takenOver_ ? " (taken over running)" : "") + ", TX lamp frames on change + 4 Hz heartbeat"
         + (lamps_ ? "" : " (no lamps)");
    return true;
}

void PanelLink::stop()
{
    quit_ = true;
    wake(this);
    if (thread_.joinable())
        thread_.join();
    if (lamps_)
        lamps_->setWake(nullptr, nullptr);
    if (fd_ >= 0)
        if (! map_.takeOver)
            driverTimerOff(fd_); // leave the device idle for the stock app (the DJS-1000's keeps running: see start())
    for (int* fd : {&fd_, &wakeRead_, &wakeWrite_})
        if (*fd >= 0)
        {
            ::close(*fd);
            *fd = -1;
        }
    running_ = false;
}

void PanelLink::wake(void* self)
{
    auto* link = static_cast<PanelLink*>(self);
    if (link->wakeWrite_ >= 0)
    {
        const char b = 1;
        (void) ! ::write(link->wakeWrite_, &b, 1);
    }
}

void PanelLink::sendDue(double now, double& waitMs)
{
    waitMs = 100.0;
    if (! lamps_)
        return;
    panel::TxFrame frame;
    if (! lamps_->take(now, frame, waitMs))
        return;
    SpiTransfer t {0, 0, 0, 1, panel::kFrameBytes, reinterpret_cast<std::uintptr_t>(frame.data())};
    if (ioctl(fd_, reqTransfer, &t) == 0)
        txFrames_.fetch_add(1, std::memory_order_relaxed);
    else
        txErrors_.fetch_add(1, std::memory_order_relaxed);
}

void PanelLink::run()
{
    std::uint8_t frame[panel::kFrameBytes];
    double waitMs = 0;
    sendDue(nowMs(), waitMs);
    while (! quit_)
    {
        pollfd p[2] = {{fd_, POLLIN, 0}, {wakeRead_, POLLIN, 0}};
        const int r = ::poll(p, wakeRead_ >= 0 ? 2 : 1, static_cast<int>(waitMs) + 1);
        if (r > 0 && wakeRead_ >= 0 && (p[1].revents & POLLIN))
        {
            char drain[16];
            while (::read(wakeRead_, drain, sizeof drain) > 0)
            {
            }
        }
        if (r > 0 && (p[0].revents & POLLIN))
        {
            const auto n = ::read(fd_, frame, sizeof frame);
            if (n != panel::kFrameBytes)
            {
                shortReads_.fetch_add(1, std::memory_order_relaxed);
                if (n < 0 && errno != EAGAIN && errno != EINTR)
                    ::usleep(10000);
            }
            else if (! panel::rxFrameOk(frame))
                badCrc_.fetch_add(1, std::memory_order_relaxed);
            else
            {
                batch_.count = 0;
                const double t = nowMs();
                decoder_.decode(frame, t, batch_); // pad hits reach the TriggerSink inside
                if (frames_.fetch_add(1, std::memory_order_relaxed) == 0 && lamps_)
                    lamps_->answered();
                // The power key: the DJS-1000's panel switches the unit off only when the program asks it to (the
                // flags byte), as the stock app does on its release. Files are written out first.
                const auto power = static_cast<std::uint8_t>(frame[map_.powerByte] & (1u << map_.powerBit));
                if (lastPowerByte_ != 0 && power == 0 && map_.txFlagsOffset >= 0 && lamps_)
                {
                    ::sync();
                    lamps_->requestStandby();
                }
                lastPowerByte_ = power;
                for (int i = 0; i < batch_.count; ++i)
                {
                    const auto& e = batch_.events[static_cast<std::size_t>(i)];
                    if (! queue_.push(e))
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                    if (lamps_)
                        lamps_->echo(e);
                    if (board_ && e.control == io::Control::AfsHpCutoff && e.kind == io::ControlEvent::Kind::Value)
                        board_->setHpCutoff(e.value); // the analog high-pass follows the pot (as in the stock firmware)
                    if (phonesLevel_ && e.control == io::Control::PhonesLevel
                        && e.kind == io::ControlEvent::Kind::Value)
                    {
                        const float x = static_cast<float>(e.value) / static_cast<float>(panel::kAnalogFullScale);
                        phonesLevel_->store(x * x, std::memory_order_relaxed);
                    }
                }
            }
        }
        sendDue(nowMs(), waitMs);
    }
}

void PanelLink::poll(std::vector<io::ControlEvent>& out)
{
    io::ControlEvent e;
    while (queue_.pop(e))
    {
        if (trace_)
            traceInput(id(), e);
        out.push_back(e);
    }
}

PanelStats PanelLink::stats() const
{
    PanelStats s;
    s.frames = frames_.load();
    s.badCrc = badCrc_.load();
    s.shortReads = shortReads_.load();
    s.txFrames = txFrames_.load();
    s.txErrors = txErrors_.load();
    s.dropped = dropped_.load();
    s.running = running_.load();
    return s;
}
} // namespace platform::imx6
