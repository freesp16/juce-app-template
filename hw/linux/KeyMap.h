// The computer keyboard → logical controls: a fixed map by US key positions, fixed pad velocity 100 with `ext.accent`
// held = 127, auto-repeat ignored for pads and buttons, and a text mode (setTextMode) where only Esc and Return keep
// their meaning. Device-independent and toolkit-independent (keys are KeyCode values); linux/X11Input.cpp feeds it.
//
// Details:
//  - Option + ↑/↓ (fine) is sent as a Turn wrapped in Shift Press/Release (SHIFT + knob = fine), because
//    ControlEvent has no fine flag; nothing is wrapped while Shift is physically held.
//  - The focused column (←/→, Control + 1…6) starts at 1 and stays within 1…6.
//  - ⌘S/⌘O/⌘N are left for application menu commands: unmapped here.
//  - `;` / `'` = ext.nudgeDown / ext.nudgeUp, held.
//  - Typed text in the text mode is the app's to handle; ControlEvent has no Text kind.
#pragma once

#include "Platform.h"

#include <array>
#include <cstdint>
#include <vector>

namespace platform::host
{
// Printable keys are their lower-case ASCII code; the others:
enum KeyCode : int
{
    kKeyF1 = 0x10001, // … kKeyF1 + 11 = F12
    kKeyLeft = 0x10020, kKeyRight, kKeyUp, kKeyDown, kKeyReturn, kKeyEscape, kKeyHome, kKeyDelete, kKeyBackspace,
};
enum KeyModifier : unsigned { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModCmd = 8 };

class KeyMap
{
public:
    // A key went down (repeat = auto-repeat) or up. Appends the logical events; pad hits also go to `triggers`
    // (the low-latency path; may be null). Returns true when the key is mapped.
    bool key(int code, unsigned mods, bool down, bool repeat, double timeMs, platform::TriggerSink* triggers,
             std::vector<io::ControlEvent>& out);
    // The modifier state changed (Shift → `shift` Press/Release).
    void modifiers(unsigned mods, double timeMs, std::vector<io::ControlEvent>& out);

    void setFixedVelocity(int v) noexcept { velocity_ = v < 1 ? 1 : (v > 127 ? 127 : v); }
    void setTextMode(bool on) noexcept { textMode_ = on; }
    int focusedColumn() const noexcept { return column_; }

private:
    struct Held
    {
        int code;
        io::Control control;
    };
    void press(int code, io::Control c, std::int16_t value, double t, std::vector<io::ControlEvent>& out);
    void turn(io::Control c, int detents, bool fine, double t, std::vector<io::ControlEvent>& out);

    std::array<Held, 32> held_ {};
    int heldCount_ = 0;
    bool shift_ = false, accent_ = false, textMode_ = false;
    int velocity_ = 100, column_ = 1;
};
} // namespace platform::host
