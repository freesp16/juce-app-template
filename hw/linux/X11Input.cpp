#include "linux/X11Input.h"

#include "linux/PointerMap.h"
#include "linux/XKeyMap.h"


#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>

namespace platform::linux_desktop
{
namespace
{
double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

X11InputSource::X11InputSource(platform::TriggerSink* triggers, std::shared_ptr<LinuxWindow> window)
    : window_(std::move(window)), triggers_(triggers)
{
}

bool X11InputSource::start(std::string& info)
{
    info = "x11: keyboard, mouse button 1 = LCD touch (800x480 points, window / "
         + std::to_string(window_->scale) + "), wheel = selector, Ctrl + wheel = touch strip, close = quit";
    return true;
}

void X11InputSource::stop() {}

void X11InputSource::releaseAll(double t, std::vector<io::ControlEvent>& out)
{
    for (auto& code : held_)
        if (code != 0)
        {
            keys_.key(code, mods_, false, false, t, triggers_, pending_);
            code = 0;
        }
    if (mods_ != 0)
        keys_.modifiers(mods_ = 0, t, pending_);
    if (const auto lift = strip_.lift(t))
        out.push_back(*lift);
    if (touching_)
    {
        io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, 0, io::kSourceKeyboard, t};
        e.x = lastX_;
        e.y = lastY_;
        out.push_back(e);
        touching_ = false;
    }
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void X11InputSource::poll(std::vector<io::ControlEvent>& out)
{
    if (! window_->x)
        return;
    auto* dpy = static_cast<Display*>(common::x11Display(*window_->x));
    if (attached_ != dpy)
    {
        Bool supported = False;
        XkbSetDetectableAutoRepeat(dpy, True, &supported); // repeats arrive as press, press, …, release
        attached_ = dpy;
    }
    const auto win = static_cast<Window>(common::x11Drawable(*window_->x));
    const auto wmDelete = static_cast<Atom>(common::x11WmDeleteAtom(*window_->x));
    const int scale = window_->scale;
    while (XPending(dpy) > 0) // never blocks: only what already arrived
    {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.xany.window != win)
            continue;
        const double t = nowMs();
        switch (ev.type)
        {
        case KeyPress:
        case KeyRelease:
        {
            const bool down = ev.type == KeyPress;
            const unsigned long ks = XLookupKeysym(&ev.xkey, 0);
            const unsigned keycode = ev.xkey.keycode & 0xFF;
            if (const unsigned bit = modifierForKeysym(ks))
            {
                mods_ = down ? (modsForState(ev.xkey.state) | bit) : (modsForState(ev.xkey.state) & ~bit);
                keys_.modifiers(mods_, t, pending_);
                if ((mods_ & host::kModCtrl) == 0)
                    if (const auto lift = strip_.lift(t)) // Ctrl up: the strip is released
                        out.push_back(*lift);
                break;
            }
            mods_ = modsForState(ev.xkey.state);
            const int code = keyCodeForKeysym(ks);
            if (code == 0)
                break;
            if (down)
            {
                const bool repeat = held_[keycode] != 0;
                held_[keycode] = code;
                keys_.key(code, mods_, true, repeat, t, triggers_, pending_);
            }
            else if (held_[keycode] != 0)
            {
                keys_.key(held_[keycode], mods_, false, false, t, triggers_, pending_);
                held_[keycode] = 0;
            }
            break;
        }
        case ButtonPress:
        case ButtonRelease:
        {
            const auto p = toLogicalPoint(ev.xbutton.x, ev.xbutton.y, scale);
            if (ev.xbutton.button == Button1)
            {
                const bool down = ev.type == ButtonPress;
                if (down == touching_)
                    break;
                touching_ = down;
                io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen,
                                    static_cast<std::int16_t>(down ? 1 : 0), io::kSourceKeyboard, t};
                e.x = lastX_ = p.x;
                e.y = lastY_ = p.y;
                out.push_back(e);
            }
            else if (ev.type == ButtonPress && (ev.xbutton.button == Button4 || ev.xbutton.button == Button5))
            {
                const auto detents = static_cast<std::int16_t>(ev.xbutton.button == Button4 ? 1 : -1); // up = +1
                if ((ev.xbutton.state & ControlMask) != 0) // Ctrl + wheel = the touch strip (StripWheel.h)
                    out.push_back(strip_.wheel(detents, t));
                else
                    out.push_back({io::ControlEvent::Kind::Turn, io::Control::Selector, detents, io::kSourceKeyboard, t});
            }
            break;
        }
        case MotionNotify:
        {
            if (! touching_)
                break;
            const auto p = toLogicalPoint(ev.xmotion.x, ev.xmotion.y, scale);
            if (p.x == lastX_ && p.y == lastY_)
                break;
            io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, 2, io::kSourceKeyboard, t};
            e.x = lastX_ = p.x;
            e.y = lastY_ = p.y;
            out.push_back(e);
            break;
        }
        case FocusOut: releaseAll(t, out); break;
        case ClientMessage:
            if (wmDelete != 0 && static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete)
                platform::requestQuit();
            break;
        case DestroyNotify: platform::requestQuit(); break;
        default: break;
        }
        out.insert(out.end(), pending_.begin(), pending_.end()); // in order with the mouse's
        pending_.clear();
    }
}
} // namespace platform::linux_desktop
