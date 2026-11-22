// Lamp state resolution and the touch strip's LED rule. See LampRules.h.
#include "controls/LampRules.h"

#include <algorithm>
#include <cmath>

namespace io
{
namespace
{
constexpr std::uint8_t brightnessScale(LampBrightness b) noexcept // out of 255
{
    switch (b)
    {
    case LampBrightness::Normal: return 255;
    case LampBrightness::Dimmed1: return 128;
    case LampBrightness::Dimmed2: return 84;
    case LampBrightness::Dimmed3: return 64;
    }
    return 255;
}

std::uint32_t blend(std::uint32_t from, std::uint32_t to, double t) noexcept // t 0 = from, 1 = to
{
    std::uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        const double a = (from >> shift) & 0xFF, b = (to >> shift) & 0xFF;
        out |= static_cast<std::uint32_t>(std::lround(a + (b - a) * t)) << shift;
    }
    return out;
}

bool blinkOn(double nowMs, double sinceMs, double hz) noexcept
{
    const double periodMs = 1000.0 / hz;
    const double phase = std::fmod(std::max(0.0, nowMs - sinceMs), periodMs);
    return phase < periodMs * 0.5;
}

LampState lit(bool b, std::uint32_t rgb) noexcept { return b ? LampState {LampMode::On, rgb, 0} : LampState {}; }
} // namespace

LampOutput resolve(const LampState& s, double nowMs, LampBrightness b) noexcept
{
    int level = 0;
    std::uint32_t rgb = s.rgb;
    switch (s.mode)
    {
    case LampMode::Off: break;
    case LampMode::On: level = 255; break;
    case LampMode::Dim: level = kDimLevel; break;
    case LampMode::Faint: level = kFaintLevel; break;
    case LampMode::BlinkSlow: level = blinkOn(nowMs, s.sinceMs, kBlinkSlowHz) ? 255 : 0; break;
    case LampMode::BlinkFast: level = blinkOn(nowMs, s.sinceMs, kBlinkFastHz) ? 255 : 0; break;
    case LampMode::Flash:
    {
        const double t = std::clamp((nowMs - s.sinceMs) / kFlashMs, 0.0, 1.0);
        rgb = blend(lampcolour::white, s.rgb, t);
        level = rgb == 0 ? 0 : 255;
        break;
    }
    }
    if (rgb == 0)
        level = 0;
    level = s.mode == LampMode::Faint ? std::max(1, level * brightnessScale(b) / 255) : level * brightnessScale(b) / 255;
    return {level == 0 ? 0u : rgb, static_cast<std::uint8_t>(level)};
}

void apply(const LampFrame& frame, double nowMs, LampBrightness b, LampSink& sink)
{
    for (const auto c : lampControls())
    {
        const auto out = resolve(frame[c], nowMs, b);
        sink.setLamp(c, out.rgb, out.level);
    }
    sink.flush();
}

int stripSegment(int position) noexcept { return std::clamp(position, 0, 127) * 8 / 128; }

void stripLamps(int position, bool contact, bool hold, StripMode mode, LampFrame& f) noexcept
{
    const bool show = contact || hold;
    const int seg = stripSegment(position);
    for (int i = 0; i < 8; ++i)
    {
        const bool l = show && (mode == StripMode::Repeat ? i == seg : i <= seg);
        f[static_cast<Control>(static_cast<int>(Control::StripLed1) + i)] = lit(l, lampcolour::white);
    }
}
} // namespace io
