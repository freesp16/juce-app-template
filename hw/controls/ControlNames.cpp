// Per-control facts (kind, lamp, value range) and the INPUT log line text. See ControlNames.h.
#include "controls/ControlNames.h"

#include <array>
#include <cstdio>
#include <utility>

namespace io
{
namespace
{
constexpr ControlInfo infoFor(Control c) noexcept
{
    using K = ControlKind;
    using L = LampKind;
    if (isPad(c))
        return {K::Pad, L::Rgb, 127, 0, true};
    if (isStep(c))
        return {K::Button, L::Rgb, 0, 0, true};
    if (isKnob(c))
        return {K::Encoder, L::None, 0, 0, true};
    if (isBarPositionLamp(c))
        return {K::LampOnly, L::Mono, 0, 0, true};
    switch (c)
    {
    case Control::Bar1:
    case Control::Bar2:
    case Control::Bar3:
    case Control::Bar4: return {K::Button, L::Mono, 0, 0, true};
    case Control::PadModeTrack:
    case Control::PadModeMute:
    case Control::PadModeSlice:
    case Control::PadModeScale: return {K::Button, L::Mono, 0, 1, true};
    case Control::Scene:
    case Control::Pattern:
    case Control::Rec:
    case Control::Stop: // STOP has no lamp in the manual, but the panel has an LED channel for it
    case Control::Play:
    case Control::StripHold:
    case Control::AfsActive: return {K::Button, L::Mono, 0, 0, true};
    case Control::Shift:
    case Control::SelectorPush:
    case Control::Back:
    case Control::Home:
    case Control::StripMode: return {K::Button, L::None, 0, 0, true};
    case Control::Selector: return {K::Encoder, L::None, 0, 0, true};
    case Control::AfsDrive:
    case Control::AfsCutoff:
    case Control::AfsResonance:
    case Control::AfsHpCutoff:
    case Control::MainVolume: return {K::Absolute, L::None, 1023, 0, true};
    case Control::Strip: return {K::Strip, L::None, 127, 0, true};
    case Control::Screen: return {K::Screen, L::None, 2, 0, true};
    case Control::StripLed1:
    case Control::StripLed2:
    case Control::StripLed3:
    case Control::StripLed4:
    case Control::StripLed5:
    case Control::StripLed6:
    case Control::StripLed7:
    case Control::StripLed8:
    case Control::AfsOver: return {K::LampOnly, L::Mono, 0, 0, true};
    case Control::StripModePitch:
    case Control::StripModeRepeat:
    case Control::StripModeUser1:
    case Control::StripModeUser2: return {K::LampOnly, L::Mono, 0, 2, true};
    // DJS-1000 panel (reference = false: not on the SP-16 panel; Platform::capabilities() says what a unit has)
    case Control::PadModeHotSlice: return {K::Button, L::Mono, 0, 1, false};
    case Control::TempoReset:
    case Control::Sync:
    case Control::Master:
    case Control::FxOn:
    case Control::LiveSampling:
    case Control::FxSelect:
    case Control::ExtProject: return {K::Button, L::Mono, 0, 0, false}; // PROJECT is lit on the DJS-1000
    case Control::TempoRange: return {K::Button, L::None, 0, 0, false};
    case Control::TempoFader:
    case Control::FxLevel:
    case Control::PhonesLevel: return {K::Absolute, L::None, 1023, 0, false};
    default: break;
    }
    if (c >= Control::ExtCopy && c <= Control::ExtProject)
        return {K::Button, L::None, 0, 0, false};
    return {K::LampOnly, L::None, 0, 0, false};
}

constexpr auto buildInputs() noexcept
{
    std::array<Control, 70> list {};
    std::size_t k = 0;
    for (int i = 1; i <= 16; ++i)
        list[k++] = pad(i);
    for (int i = 1; i <= 16; ++i)
        list[k++] = step(i);
    for (auto c : {Control::Bar1, Control::Bar2, Control::Bar3, Control::Bar4, Control::PadModeTrack,
                   Control::PadModeMute, Control::PadModeSlice, Control::PadModeScale, Control::Shift, Control::Rec,
                   Control::Stop, Control::Play, Control::Scene, Control::Pattern})
        list[k++] = c;
    for (int i = 1; i <= 6; ++i)
        list[k++] = knob(i);
    for (auto c : {Control::Selector, Control::SelectorPush, Control::Back, Control::Home, Control::Strip,
                   Control::StripMode, Control::StripHold, Control::AfsActive, Control::AfsDrive, Control::AfsCutoff,
                   Control::AfsResonance, Control::AfsHpCutoff, Control::MainVolume, Control::Screen})
        list[k++] = c;
    return std::pair {list, k};
}
constexpr auto inputs = buildInputs();

constexpr auto buildLamps() noexcept
{
    std::array<Control, kLampSlots> list {};
    std::size_t k = 0;
    for (int i = 0; i < kLampSlots; ++i)
        if (infoFor(static_cast<Control>(i)).lamp != LampKind::None)
            list[k++] = static_cast<Control>(i);
    return std::pair {list, k};
}
constexpr auto lamps = buildLamps();
} // namespace

ControlInfo controlInfo(Control c) noexcept { return infoFor(c); }

std::span<const Control> referenceInputControls() noexcept { return {inputs.first.data(), inputs.second}; }
std::span<const Control> lampControls() noexcept { return {lamps.first.data(), lamps.second}; }

std::string_view lampName(Control c) noexcept
{
    static constexpr std::string_view positions[] = {"bar.position.1", "bar.position.2", "bar.position.3",
                                                     "bar.position.4"};
    if (isBarPositionLamp(c))
        return positions[static_cast<int>(c) - static_cast<int>(barPositionLamp(1))];
    return nameOf(c);
}

std::string_view kindName(ControlEvent::Kind k) noexcept
{
    switch (k)
    {
    case ControlEvent::Kind::Press: return "press";
    case ControlEvent::Kind::Release: return "release";
    case ControlEvent::Kind::Turn: return "turn";
    case ControlEvent::Kind::Value: return "value";
    case ControlEvent::Kind::Touch: return "touch";
    case ControlEvent::Kind::Pressure: return "pressure";
    }
    return "?";
}

std::size_t describe(const ControlEvent& e, char* out, std::size_t n) noexcept
{
    if (n == 0)
        return 0;
    const auto name = lampName(e.control);
    const auto kind = kindName(e.kind);
    int written = 0;
    if (e.control == Control::Screen)
        written = std::snprintf(out, n, "%.*s %.*s %d @%d,%d", static_cast<int>(name.size()), name.data(),
                                static_cast<int>(kind.size()), kind.data(), e.value, e.x, e.y);
    else
        written = std::snprintf(out, n, "%.*s %.*s %d", static_cast<int>(name.size()), name.data(),
                                static_cast<int>(kind.size()), kind.data(), e.value);
    if (written < 0)
    {
        out[0] = 0;
        return 0;
    }
    return static_cast<std::size_t>(written) < n ? static_cast<std::size_t>(written) : n - 1;
}
} // namespace io
