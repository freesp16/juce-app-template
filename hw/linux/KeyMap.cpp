// The default keyboard map. See KeyMap.h.
#include "linux/KeyMap.h"

#include <cstring>

namespace platform::host
{
namespace
{
using io::Control;

// Shifted US symbols and capitals → the key position they share (the map is by key position).
int normalise(int code) noexcept
{
    if (code >= 'A' && code <= 'Z')
        return code - 'A' + 'a';
    constexpr const char* from = "!@#$%^&*()_+{}<>~:\"";
    constexpr const char* to = "1234567890-=[],.`;'";
    if (code > 0 && code < 128)
        if (const char* p = std::strchr(from, code); p != nullptr && *p != 0)
            return to[p - from];
    return code;
}

// Plain keys (no Command/Control): one control each.
Control plainControl(int code) noexcept
{
    switch (code)
    {
    case '1': return io::pad(13);
    case '2': return io::pad(14);
    case '3': return io::pad(15);
    case '4': return io::pad(16);
    case 'q': return io::pad(9);
    case 'w': return io::pad(10);
    case 'e': return io::pad(11);
    case 'r': return io::pad(12);
    case 'a': return io::pad(5);
    case 's': return io::pad(6);
    case 'd': return io::pad(7);
    case 'f': return io::pad(8);
    case 'z': return io::pad(1);
    case 'x': return io::pad(2);
    case 'c': return io::pad(3);
    case 'v': return io::pad(4);
    case '5': return io::step(1);
    case '6': return io::step(2);
    case '7': return io::step(3);
    case '8': return io::step(4);
    case '9': return io::step(5);
    case '0': return io::step(6);
    case '-': return io::step(7);
    case '=': return io::step(8);
    case 't': return io::step(9);
    case 'y': return io::step(10);
    case 'u': return io::step(11);
    case 'i': return io::step(12);
    case 'o': return io::step(13);
    case 'p': return io::step(14);
    case '[': return io::step(15);
    case ']': return io::step(16);
    case ' ': return Control::Play;
    case 'b': return Control::Stop;
    case 'g': return Control::Rec;
    case 'n': return Control::ExtTap;
    case 'm': return Control::ExtCue;
    case 'j': return Control::ExtErase;
    case 'k': return Control::ExtAccent;
    // NUDGE (held): the two keys right of L (US layout).
    case ';': return Control::ExtNudgeDown;
    case '\'': return Control::ExtNudgeUp;
    case '`': return Control::Home;
    case kKeyHome: return Control::Home;
    case kKeyEscape: return Control::Back;
    case kKeyReturn: return Control::SelectorPush;
    case kKeyDelete:
    case kKeyBackspace: return Control::ExtClear;
    default: break;
    }
    if (code >= kKeyF1 && code < kKeyF1 + 12)
    {
        constexpr Control f[] = {Control::Bar1,         Control::Bar2,         Control::Bar3,
                                 Control::Bar4,         Control::PadModeTrack, Control::PadModeMute,
                                 Control::PadModeSlice, Control::PadModeScale, Control::Scene,
                                 Control::Pattern,      Control::StripMode,    Control::StripHold};
        return f[code - kKeyF1];
    }
    return Control::Count;
}

Control commandControl(int code, unsigned mods) noexcept
{
    switch (code)
    {
    case '1': return Control::ExtMixer;
    case '2': return Control::ExtBrowse;
    case '3': return Control::ExtUtility;
    case '4': return Control::ExtProject;
    case 'c': return Control::ExtCopy;
    case 'v': return Control::ExtPaste;
    case 'z': return (mods & kModShift) != 0 ? Control::ExtRedo : Control::ExtUndo;
    default: return Control::Count;
    }
}
} // namespace

void KeyMap::press(int code, Control c, std::int16_t value, double t, std::vector<io::ControlEvent>& out)
{
    if (heldCount_ < static_cast<int>(held_.size()))
        held_[static_cast<std::size_t>(heldCount_++)] = {code, c};
    out.push_back({io::ControlEvent::Kind::Press, c, value, io::kSourceKeyboard, t});
}

void KeyMap::turn(Control c, int detents, bool fine, double t, std::vector<io::ControlEvent>& out)
{
    using Kind = io::ControlEvent::Kind;
    const bool wrap = fine && ! shift_;
    if (wrap)
        out.push_back({Kind::Press, Control::Shift, 0, io::kSourceKeyboard, t});
    out.push_back({Kind::Turn, c, static_cast<std::int16_t>(detents), io::kSourceKeyboard, t});
    if (wrap)
        out.push_back({Kind::Release, Control::Shift, 0, io::kSourceKeyboard, t});
}

void KeyMap::modifiers(unsigned mods, double t, std::vector<io::ControlEvent>& out)
{
    const bool shift = (mods & kModShift) != 0;
    if (shift == shift_)
        return;
    shift_ = shift;
    out.push_back({shift ? io::ControlEvent::Kind::Press : io::ControlEvent::Kind::Release, Control::Shift, 0,
                   io::kSourceKeyboard, t});
}

bool KeyMap::key(int code, unsigned mods, bool down, bool repeat, double t, platform::TriggerSink* triggers,
                 std::vector<io::ControlEvent>& out)
{
    code = normalise(code);
    modifiers(mods, t, out);
    if (! down)
    {
        for (int i = 0; i < heldCount_; ++i)
        {
            auto& h = held_[static_cast<std::size_t>(i)];
            if (h.code != code)
                continue;
            const auto c = h.control;
            h = held_[static_cast<std::size_t>(--heldCount_)];
            if (c == Control::ExtAccent)
                accent_ = false;
            out.push_back({io::ControlEvent::Kind::Release, c, 0, io::kSourceKeyboard, t});
            return true;
        }
        return false;
    }

    // Text mode: only Esc and Return keep their meaning.
    if (textMode_)
    {
        if (code != kKeyEscape && code != kKeyReturn)
            return false;
        bool isDown = false;
        for (int i = 0; i < heldCount_; ++i)
            isDown = isDown || held_[static_cast<std::size_t>(i)].code == code;
        if (! repeat && ! isDown)
            press(code, plainControl(code), 0, t, out);
        return true;
    }

    // Encoders: repeats allowed (a held arrow keeps turning).
    if (code == kKeyUp || code == kKeyDown)
    {
        turn(io::knob(column_), code == kKeyUp ? 1 : -1, (mods & kModAlt) != 0, t, out);
        return true;
    }
    if (code == kKeyLeft || code == kKeyRight)
    {
        column_ = code == kKeyLeft ? (column_ > 1 ? column_ - 1 : 1) : (column_ < 6 ? column_ + 1 : 6);
        return true;
    }
    if ((code == ',' || code == '.') && (mods & (kModCmd | kModCtrl)) == 0)
    {
        turn(Control::Selector, code == '.' ? 1 : -1, false, t, out);
        return true;
    }
    if ((mods & kModCtrl) != 0 && code >= '1' && code <= '6')
    {
        column_ = code - '0';
        return true;
    }

    Control c = (mods & kModCmd) != 0 ? commandControl(code, mods)
              : (mods & kModCtrl) != 0 ? Control::Count
                                       : plainControl(code);
    if (c == Control::Count)
        return false;
    if (repeat)
        return true; // auto-repeat ignored for pads and buttons
    for (int i = 0; i < heldCount_; ++i)
        if (held_[static_cast<std::size_t>(i)].code == code)
            return true; // already down (a repeat that did not say so)
    if (io::isPad(c))
    {
        const int velocity = accent_ ? 127 : velocity_;
        if (triggers != nullptr) // the low-latency path
            triggers->push({static_cast<std::uint8_t>(io::indexOf(c) - 1), static_cast<std::uint8_t>(velocity), 60,
                            static_cast<std::int8_t>(io::indexOf(c) - 1)});
        press(code, c, static_cast<std::int16_t>(velocity), t, out);
        return true;
    }
    if (c == Control::ExtAccent)
        accent_ = true;
    press(code, c, 0, t, out);
    return true;
}
} // namespace platform::host
