// The touch strip's desktop stand-in on the linux window: Ctrl + mouse
// wheel plays the strip. The first Ctrl + wheel detent touches it at the last position (64 = centre at start) moved by
// one step, every further detent moves it by kStep (up = towards the top, 16 detents bottom → top), and releasing Ctrl
// lifts the finger (Release at the last position). Without Ctrl the wheel stays the Selector. Events are
// Control::Strip Touch / Value / Release from kSourceKeyboard, so the app passes them on to its audio side itself
// (the unit's strip reader thread pushes to the TriggerSink directly). Header-only and toolkit-independent.
#pragma once

#include "Platform.h"

#include <cstdint>
#include <optional>

namespace platform::linux_desktop
{
class StripWheel
{
public:
    static constexpr int kStep = 8;

    // One wheel detent (+1 up, −1 down) while Ctrl is held.
    io::ControlEvent wheel(int detents, double timeMs) noexcept
    {
        int p = position_ + detents * kStep;
        p = p < 0 ? 0 : (p > 127 ? 127 : p);
        position_ = p;
        const auto kind = touching_ ? io::ControlEvent::Kind::Value : io::ControlEvent::Kind::Touch;
        touching_ = true;
        return {kind, io::Control::Strip, static_cast<std::int16_t>(p), io::kSourceKeyboard, timeMs};
    }
    // Ctrl went up (or the window lost the focus): the finger leaves the strip.
    std::optional<io::ControlEvent> lift(double timeMs) noexcept
    {
        if (! touching_)
            return std::nullopt;
        touching_ = false;
        return io::ControlEvent {io::ControlEvent::Kind::Release, io::Control::Strip, static_cast<std::int16_t>(position_),
                                 io::kSourceKeyboard, timeMs};
    }
    bool touching() const noexcept { return touching_; }
    int position() const noexcept { return position_; }

private:
    int position_ = 64;
    bool touching_ = false;
};
} // namespace platform::linux_desktop
