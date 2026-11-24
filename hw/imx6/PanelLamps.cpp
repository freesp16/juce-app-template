// Panel lamps and the TX schedule (see PanelLamps.h). Device-independent: also compiles on a desktop.
#include "imx6/PanelLamps.h"

#include <algorithm>

namespace platform::imx6::panel
{
namespace
{
std::uint32_t echoColour(io::Control c) noexcept
{
    switch (c)
    {
    case io::Control::Rec: return io::lampcolour::red;
    case io::Control::Play: return io::lampcolour::green;
    case io::Control::AfsActive: return io::lampcolour::red;
    default: return io::lampcolour::white;
    }
}
} // namespace

LampBus::LampBus(platform::Unit unit) : unit_(unit), frame_(unit)
{
    echo_[io::Control::StripModePitch] = {io::LampMode::On, io::lampcolour::white, 0};
    renderEcho(0);
}

void LampBus::setWake(void (*wake)(void*), void* arg) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = wake;
    wakeArg_ = arg;
}

void LampBus::publish(const TxFrame& f)
{
    void (*wake)(void*) = nullptr;
    void* arg = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        external_.store(true, std::memory_order_release);
        if (! (f == frame_))
        {
            frame_ = f;
            changed_ = true;
            wake = wake_;
            arg = wakeArg_;
        }
    }
    if (wake != nullptr)
        wake(arg);
}

void LampBus::echo(const io::ControlEvent& e)
{
    if (external())
        return;
    using Kind = io::ControlEvent::Kind;
    void (*wake)(void*) = nullptr;
    void* arg = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto info = io::controlInfo(e.control);
        if (io::isPad(e.control) && e.kind == Kind::Press)
        {
            echo_[e.control] = {io::LampMode::Flash, 0, e.timeMs}; // white → dark over 1 s
            echoAnimUntilMs_ = std::max(echoAnimUntilMs_, e.timeMs + io::kFlashMs);
        }
        else if (info.kind == io::ControlKind::Button && info.lamp != io::LampKind::None
                 && (e.kind == Kind::Press || e.kind == Kind::Release))
            echo_[e.control] = {e.kind == Kind::Press ? io::LampMode::On : io::LampMode::Off, echoColour(e.control), 0};
        else if (e.control == io::Control::Strip)
            io::stripLamps(e.value, e.kind != Kind::Release, false, io::StripMode::Pitch, echo_);
        else
            return;
        echoDirty_ = true; // rendered once by the next take(), however many events a frame carries
        wake = wake_;
        arg = wakeArg_;
    }
    if (wake != nullptr)
        wake(arg);
}

void LampBus::renderEcho(double nowMs)
{
    lastEchoRenderMs_ = nowMs;
    echoDirty_ = false;
    TxFrame f(unit_);
    for (const auto c : io::lampControls())
    {
        const auto out = io::resolve(echo_[c], nowMs);
        f.setLamp(c, out.rgb, out.level);
    }
    f.finish(flags());
    if (! (f == frame_))
    {
        frame_ = f;
        changed_ = true;
    }
}

bool LampBus::take(double nowMs, TxFrame& out, double& waitMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A running echo fade is recomputed until one render lands after its end.
    const bool fading = lastEchoRenderMs_ <= echoAnimUntilMs_;
    if (! external() && (echoDirty_ || (fading && nowMs - lastEchoRenderMs_ >= kEchoTickMs)))
        renderEcho(nowMs);
    const bool animating = ! external() && lastEchoRenderMs_ <= echoAnimUntilMs_;
    const double since = nowMs - lastSentMs_;
    const bool due = (changed_ && since >= kMinGapMs) || since >= kHeartbeatMs;
    if (due)
    {
        out = frame_;
        out.finish(flags()); // the DJS-1000's flags as they are now
        changed_ = false;
        lastSentMs_ = nowMs;
    }
    const double nextBeat = kHeartbeatMs - (nowMs - lastSentMs_);
    waitMs = animating ? std::min(kEchoTickMs, nextBeat) : (changed_ ? kMinGapMs - since : nextBeat);
    if (waitMs < 1.0)
        waitMs = 1.0;
    return due;
}

std::uint8_t LampBus::flags() const noexcept
{
    return static_cast<std::uint8_t>((outputsLive_.load() ? 0 : TxFrame::kFlagMute)
                                     | (standby_.load() ? TxFrame::kFlagStandby : 0)
                                     | (answered_.load() ? 0 : TxFrame::kFlagStartup));
}

// A change of the flags is sent at once (as a changed frame), whoever makes it.
static void flagChanged(std::atomic<bool>& flag, bool value, std::mutex& mutex, bool& changed, void (*&wake)(void*),
                        void*& arg)
{
    if (flag.exchange(value) == value)
        return;
    void (*w)(void*) = nullptr;
    void* a = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        changed = true;
        w = wake;
        a = arg;
    }
    if (w != nullptr)
        w(a);
}

void LampBus::setOutputsLive(bool live) { flagChanged(outputsLive_, live, mutex_, changed_, wake_, wakeArg_); }
void LampBus::answered() { flagChanged(answered_, true, mutex_, changed_, wake_, wakeArg_); }
void LampBus::requestStandby() { flagChanged(standby_, true, mutex_, changed_, wake_, wakeArg_); }

void PanelLamps::flush()
{
    frame_.finish(bus_->flags());
    bus_->publish(frame_);
    if (board_)
        board_->setActive(active_);
}
} // namespace platform::imx6::panel
