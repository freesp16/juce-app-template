// The linux window's input as one io::ControlSource ("x11-input"), polled on the UI thread: it drains the
// shared window's X connection without blocking (XPending / XNextEvent) — the renderer uses the same connection on
// the same thread — and produces:
//  - keyboard: X keysyms → the KeyMap codes (linux/XKeyMap.h) through KeyMap's toolkit-independent
//    key(code, mods, down, repeat, t); pad hits go straight to the app's TriggerSink.
//    Auto-repeat is detected with XkbSetDetectableAutoRepeat (a repeat = a second press without a release); keys
//    held when the window loses focus are released.
//  - mouse: button 1 = LCD touch (Kind::Touch on Control::Screen: 1 down, 2 drag while held, 0 up) at the logical
//    800 × 480 point (window pixels / scale); the wheel turns the Selector (up = +1 detent); Ctrl + wheel plays
//    the touch strip, releasing Ctrl lifts it (StripWheel.h).
//  - window: the close button (WM_DELETE_WINDOW) asks the app to quit (platform::requestQuit).
// Before a renderer opened the window there is nothing to read (poll() returns nothing).
#pragma once

#include "linux/KeyMap.h"
#include "linux/LinuxDisplay.h"
#include "linux/StripWheel.h"

#include "Platform.h"

#include <array>
#include <memory>

namespace platform::linux_desktop
{
class X11InputSource final : public io::ControlSource
{
public:
    X11InputSource(platform::TriggerSink* triggers, std::shared_ptr<LinuxWindow> window);
    std::string_view id() const override { return "x11-input"; }
    bool start(std::string& info) override;
    void poll(std::vector<io::ControlEvent>& out) override;
    void stop() override;

private:
    void releaseAll(double t, std::vector<io::ControlEvent>& out);

    std::shared_ptr<LinuxWindow> window_;
    platform::TriggerSink* triggers_;
    platform::host::KeyMap keys_;           // keyboard → controls (linux/KeyMap.cpp)
    std::vector<io::ControlEvent> pending_; // the keys' events, in order with the mouse's
    void* attached_ = nullptr;       // the Display* auto-repeat detection was set on
    std::array<int, 256> held_ {};   // X keycode → KeyMap code while the key is down (0 = up)
    unsigned mods_ = 0;
    bool touching_ = false;
    StripWheel strip_; // Ctrl + wheel = the touch strip
    std::int16_t lastX_ = -1, lastY_ = -1;
};
} // namespace platform::linux_desktop
