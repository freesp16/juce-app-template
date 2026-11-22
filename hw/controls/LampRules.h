// Lamp and colour feedback rules: the lamp states (off, on, dim, blink-slow 2 Hz, blink-fast 6 Hz, flash = on then
// fade), the fixed signal colours, the brightness preference, and the touch strip's LED rule. The app (message thread,
// about 60 Hz) fills a LampFrame and calls apply(), which resolves every state at `nowMs` and sends it to a LampSink
// (the panel's LEDs, a virtual panel, a MIDI controller).
//
// Device-independent and allocation-free; resolve() is a few multiplies per lamp.
#pragma once

#include "controls/ControlNames.h"
#include "controls/ControlSurface.h"

#include <array>
#include <cstdint>
#include <span>

namespace io
{
enum class LampMode : std::uint8_t { Off, On, Dim, BlinkSlow, BlinkFast, Flash, Faint };

struct LampState
{
    LampMode mode = LampMode::Off;
    std::uint32_t rgb = 0xFFFFFF; // colour (mono lamps: any non-zero colour = lit)
    double sinceMs = 0;           // flash start; blink phase origin
};

struct LampOutput
{
    std::uint32_t rgb;  // 0xRRGGBB
    std::uint8_t level; // 0 = off … 255 = full (LampSink::setLamp's level)
};

enum class LampBrightness : std::uint8_t { Normal, Dimmed1, Dimmed2, Dimmed3 }; // ≈ 100, 50, 33, 25 %

namespace lampcolour // fixed signal colours (chosen values; the original firmware's were not recovered)
{
inline constexpr std::uint32_t white = 0xFFFFFF, blue = 0x2050FF, red = 0xFF1E28, green = 0x28F03C, amber = 0xFFB020;
}

inline constexpr double kBlinkSlowHz = 2.0, kBlinkFastHz = 6.0;
inline constexpr double kFlashMs = 1000.0;                      // back to the base colour over about 1 s
inline constexpr std::uint8_t kDimLevel = 90;                   // "dim" ≈ 35 % of full
// "faint": the smallest light that still shows (hardware feedback: muted pads "just a small light"); 6/255 is 2 of the
// panel's 74 RGB steps and stays ≥ 1 step at the lowest brightness preference (PanelCodec never rounds a lit channel
// down to 0).
inline constexpr std::uint8_t kFaintLevel = 6;

// The level of a lamp state at `nowMs`, scaled by the brightness preference. Flash fades from
// white to the state's colour over kFlashMs; blinks are 50 % duty from sinceMs.
LampOutput resolve(const LampState&, double nowMs, LampBrightness = LampBrightness::Normal) noexcept;

// One state per lamp id (controls + bar position lamps).
struct LampFrame
{
    std::array<LampState, kLampSlots> lamps {};
    LampState& operator[](Control c) noexcept { return lamps[static_cast<std::size_t>(lampSlot(c))]; }
    const LampState& operator[](Control c) const noexcept { return lamps[static_cast<std::size_t>(lampSlot(c))]; }
};

// Resolves every lamp of lampControls() and sends it to the sink, then flush() (message thread).
void apply(const LampFrame&, double nowMs, LampBrightness, LampSink&);

// ---- the touch strip's 8 LEDs ----

enum class StripMode : std::uint8_t { Pitch, Repeat, User1, User2 };
// The touched position (or the held one): lit up to its segment in PITCH and USER modes, one segment in REPEAT.
// position 0…127; dark without contact and without hold.
void stripLamps(int position, bool contact, bool hold, StripMode, LampFrame&) noexcept;
int stripSegment(int position) noexcept; // 0…7
} // namespace io
