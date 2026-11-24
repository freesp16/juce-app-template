// The two panels' maps (PanelRoles.h) and what each unit has (Platform.h panelCapabilities). Device-independent: the
// desktop build compiles it too, to stand in for either unit.
#include "imx6/PanelRoles.h"

namespace platform::imx6::panel
{
namespace
{
const PanelMap kSp16 {
    platform::Unit::Sp16,
    true, 5, &sp16::kButtonGrid, sp16::kButtons, sp16::kSystemButtons, 5, 4,
    sp16::kCounters, sp16::kAnalog,
    0x06, 0x3f,       // pad pressure words, contact words
    0x2c, 0x2e, {},   // touch
    {},               // PadCalibration defaults (PadCurve.h)
    sp16::kLamps, MonoEncoding::BitAndDimFlag, sp16::kMono, {},
    0x6e, 0x77, 0xaa, -1,
    3, false,
};

// DJS-1000 pads: a hit begins around the per-unit calibration word (217–454, about 336 on average) and saturates near
// 898; the contact word is above 59 while a pad is held, as on the SP-16.
constexpr io::PadCalibration kDjsPads {336, 340, 898, 60};

const PanelMap kDjs1000 {
    platform::Unit::Djs1000,
    false, 6, &djs1000::kButtonGrid, djs1000::kButtons, djs1000::kSystemButtons, 5, 4,
    djs1000::kCounters, djs1000::kAnalog,
    0x16, 0x36,       // pad force words, hold words
    0x5e, 0x60, {},   // touch: the SP-16's calibration until checked on a unit
    kDjsPads,
    djs1000::kLamps, MonoEncoding::TwoBit, {}, djs1000::kTwoBit,
    0x6d, -1, 0, 0x6c,
    2, true,
};
} // namespace

const PanelMap& panelMap(platform::Unit u) noexcept { return u == platform::Unit::Djs1000 ? kDjs1000 : kSp16; }
} // namespace platform::imx6::panel

platform::Capabilities platform::panelCapabilities(Unit unit)
{
    const auto& m = imx6::panel::panelMap(unit);
    Capabilities c;
    c.unit = unit;
    c.model = std::string(unitModel(unit));
    auto mark = [](std::array<bool, io::kLampSlots>& set, io::Control ctl) {
        const int s = io::lampSlot(ctl);
        if (s >= 0 && s < io::kLampSlots)
            set[static_cast<std::size_t>(s)] = true;
    };
    for (int i = 1; i <= 16; ++i)
        mark(c.controls, io::pad(i));
    for (const auto& b : m.buttonList)
        mark(c.controls, b.control);
    for (const auto& k : m.counters)
        mark(c.controls, k.control);
    for (const auto& a : m.analog)
        mark(c.controls, a.control);
    mark(c.controls, io::Control::Strip);  // both units have the touch strip
    mark(c.controls, io::Control::Screen); // and the touch screen
    for (const auto& l : m.lamps)
        mark(c.lamps, l.control);
    return c;
}
