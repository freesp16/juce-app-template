// Text control names ("pad.1", "enc.3") ↔ io::Control.
#include "controls/ControlSurface.h"

#include <array>
#include <charconv>
#include <string>

namespace io
{
namespace
{
struct Named
{
    Control control;
    std::string_view spec, enumName;
};

// Single controls (groups pad/step/bar/enc/strip.led are parsed by prefix below).
constexpr Named singles[] = {
    {Control::PadModeTrack, "padmode.track", "PadModeTrack"}, {Control::PadModeMute, "padmode.mute", "PadModeMute"},
    {Control::PadModeSlice, "padmode.slice", "PadModeSlice"}, {Control::PadModeScale, "padmode.scale", "PadModeScale"},
    {Control::Scene, "scene", "Scene"}, {Control::Pattern, "pattern", "Pattern"},
    {Control::Selector, "sel", "Selector"}, {Control::SelectorPush, "sel.push", "SelectorPush"},
    {Control::Play, "play", "Play"}, {Control::Stop, "stop", "Stop"}, {Control::Rec, "rec", "Rec"},
    {Control::Shift, "shift", "Shift"}, {Control::Back, "back", "Back"}, {Control::Home, "home", "Home"},
    {Control::StripMode, "strip.mode", "StripMode"}, {Control::StripHold, "strip.hold", "StripHold"},
    {Control::AfsActive, "afs.active", "AfsActive"}, {Control::AfsDrive, "afs.drive", "AfsDrive"},
    {Control::AfsCutoff, "afs.cutoff", "AfsCutoff"}, {Control::AfsResonance, "afs.resonance", "AfsResonance"},
    {Control::AfsHpCutoff, "afs.hpCutoff", "AfsHpCutoff"}, {Control::MainVolume, "main.volume", "MainVolume"},
    {Control::StripModePitch, "stripmode.pitch", "StripModePitch"},
    {Control::StripModeRepeat, "stripmode.repeat", "StripModeRepeat"},
    {Control::StripModeUser1, "stripmode.user1", "StripModeUser1"},
    {Control::StripModeUser2, "stripmode.user2", "StripModeUser2"}, {Control::AfsOver, "afs.over", "AfsOver"},
    {Control::ExtCopy, "ext.copy", "ExtCopy"}, {Control::ExtPaste, "ext.paste", "ExtPaste"},
    {Control::ExtClear, "ext.clear", "ExtClear"}, {Control::ExtUndo, "ext.undo", "ExtUndo"},
    {Control::ExtRedo, "ext.redo", "ExtRedo"}, {Control::ExtCue, "ext.cue", "ExtCue"},
    {Control::ExtErase, "ext.erase", "ExtErase"}, {Control::ExtTap, "ext.tap", "ExtTap"},
    {Control::ExtNudgeDown, "ext.nudgeDown", "ExtNudgeDown"}, {Control::ExtNudgeUp, "ext.nudgeUp", "ExtNudgeUp"},
    {Control::ExtAccent, "ext.accent", "ExtAccent"}, {Control::ExtMixer, "ext.mixer", "ExtMixer"},
    {Control::ExtBrowse, "ext.browse", "ExtBrowse"}, {Control::ExtUtility, "ext.utility", "ExtUtility"},
    {Control::ExtProject, "ext.project", "ExtProject"},
    {Control::PadModeHotSlice, "padmode.hotslice", "PadModeHotSlice"}, {Control::TempoFader, "tempo", "TempoFader"},
    {Control::TempoReset, "tempo.reset", "TempoReset"}, {Control::TempoRange, "tempo.range", "TempoRange"},
    {Control::Sync, "sync", "Sync"}, {Control::Master, "master", "Master"}, {Control::FxOn, "fx.on", "FxOn"},
    {Control::FxLevel, "fx.level", "FxLevel"}, {Control::PhonesLevel, "phones.level", "PhonesLevel"},
    {Control::LiveSampling, "livesampling", "LiveSampling"}, {Control::FxSelect, "fx", "FxSelect"},
    {Control::Strip, "strip", "Strip"},
    {Control::Screen, "screen", "Screen"},
    // aliases accepted on input only (nameOf never returns them)
    {Control::Selector, "selector", "Sel"}, {Control::SelectorPush, "selector.push", "SelPush"},
};

struct Group
{
    Control first;
    int count;
    std::string_view specPrefix, enumPrefix; // "pad." / "Pad"
};
constexpr Group groups[] = {
    {Control::Pad1, 16, "pad.", "Pad"},
    {Control::Step1, 16, "step.", "Step"},
    {Control::Bar1, 4, "bar.", "Bar"},
    {Control::Knob1, 6, "enc.", "Knob"},
    {Control::StripLed1, 8, "strip.led.", "StripLed"},
};

// Text names of every group member, built once (nameOf returns views into this table).
struct GroupNames
{
    std::array<std::array<char, 16>, 50> text {};
    std::array<std::uint8_t, 50> length {};
    GroupNames()
    {
        int k = 0;
        for (const auto& g : groups)
            for (int i = 1; i <= g.count; ++i, ++k)
            {
                const std::string s = std::string(g.specPrefix) + std::to_string(i);
                s.copy(text[static_cast<std::size_t>(k)].data(), s.size());
                length[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(s.size());
            }
    }
};
const GroupNames& groupNames()
{
    static const GroupNames names;
    return names;
}

std::optional<int> numberAfter(std::string_view name, std::string_view prefix) noexcept
{
    if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix)
        return std::nullopt;
    const auto digits = name.substr(prefix.size());
    int n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc {} || end != digits.data() + digits.size())
        return std::nullopt;
    return n;
}
} // namespace

std::optional<Control> controlFromName(std::string_view name) noexcept
{
    for (const auto& g : groups)
        for (auto prefix : {g.specPrefix, g.enumPrefix})
            if (const auto n = numberAfter(name, prefix); n && *n >= 1 && *n <= g.count)
                return static_cast<Control>(static_cast<int>(g.first) + *n - 1);
    for (const auto& s : singles)
        if (s.spec == name || s.enumName == name)
            return s.control;
    return std::nullopt;
}

std::string_view nameOf(Control c) noexcept
{
    int k = 0;
    for (const auto& g : groups)
    {
        const int i = static_cast<int>(c) - static_cast<int>(g.first);
        if (i >= 0 && i < g.count)
        {
            const auto& names = groupNames();
            const auto index = static_cast<std::size_t>(k + i);
            return {names.text[index].data(), names.length[index]};
        }
        k += g.count;
    }
    for (const auto& s : singles)
        if (s.control == c)
            return s.spec;
    return {};
}
} // namespace io
