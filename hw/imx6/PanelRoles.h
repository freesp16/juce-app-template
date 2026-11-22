// The SP-16 panel's wire map as compiled tables: button roles, counters, analog words, pads, LCD touch, lamp
// channels. Worked out by observing the panel link (firmware 1.60), not taken from firmware code.
//
// Device-independent (no POSIX), so it also compiles on a desktop.
#pragma once

#include "controls/ControlNames.h"
#include "Platform.h"

#include <array>
#include <cstdint>

namespace platform::imx6::panel
{
constexpr int kFrameBytes = 128;
constexpr std::uint8_t kRxHeader = 0x01, kTxHeader = 0x02;
constexpr int kCrcOffset = 124;                         // CRC-16/X-25 over bytes 0…123, little-endian
constexpr int kPadPressureOffset = 0x06, kPadContactOffset = 0x3f;
constexpr int kTouchXOffset = 0x2c, kTouchYOffset = 0x2e;
constexpr int kTxMarkerOffset = 0x77;
constexpr std::uint8_t kTxMarker = 0xaa;
constexpr int kTxBrightnessOffset = 0x6e;                // PAD/BUTTON BRIGHTNESS 1…4 (NORMAL = 4)
constexpr int kRgbFullScale = 74;                       // lamp bytes reach 0x4a at full

struct ButtonRole
{
    std::uint8_t byte, bit;
    io::Control control; // Control::Count = a system key with no logical control (USB STOP, power)
};
inline constexpr ButtonRole kButtons[] = {
    {1, 1, io::Control::SelectorPush}, {1, 2, io::Control::Back},        {1, 3, io::Control::Home},
    {1, 4, io::Control::PadModeTrack}, {1, 5, io::Control::PadModeMute}, {1, 6, io::Control::PadModeSlice},
    {1, 7, io::Control::PadModeScale}, {2, 0, io::Control::Shift},       {2, 1, io::Control::Rec},
    {2, 2, io::Control::Stop},         {2, 3, io::Control::Play},        {2, 4, io::Control::StripHold},
    {2, 5, io::Control::StripMode},    {2, 6, io::Control::Scene},       {2, 7, io::Control::Pattern},
    {3, 0, io::Control::Step1},        {3, 1, io::Control::Step2},       {3, 2, io::Control::Step3},
    {3, 3, io::Control::Step4},        {3, 4, io::Control::Step5},       {3, 5, io::Control::Step6},
    {3, 6, io::Control::Step7},        {3, 7, io::Control::Step8},       {4, 0, io::Control::Step9},
    {4, 1, io::Control::Step10},       {4, 2, io::Control::Step11},      {4, 3, io::Control::Step12},
    {4, 4, io::Control::Step13},       {4, 5, io::Control::Step14},      {4, 6, io::Control::Step15},
    {4, 7, io::Control::Step16},       {5, 0, io::Control::Bar1},        {5, 1, io::Control::Bar2},
    {5, 2, io::Control::Bar3},         {5, 3, io::Control::Bar4},        {5, 5, io::Control::AfsActive},
};
// System keys (no logical control): reported nowhere yet.
inline constexpr ButtonRole kSystemButtons[] = {{5, 6, io::Control::Count} /* usb_stop */,
                                                {5, 4, io::Control::Count} /* power */};

// Role lookup by (byte 1…5, bit): Control::Count = no role.
struct ButtonGrid
{
    std::array<std::array<io::Control, 8>, 6> at {};
    constexpr ButtonGrid()
    {
        for (auto& row : at)
            row.fill(io::Control::Count);
        for (const auto& b : kButtons)
            at[b.byte][b.bit] = b.control;
    }
};
inline constexpr ButtonGrid kButtonGrid {};

struct Counter
{
    std::uint8_t offset;
    std::int8_t sign; // wire sign of a clockwise detent
    io::Control control;
};
inline constexpr Counter kCounters[] = {
    {48, 1, io::Control::Knob1}, {50, 1, io::Control::Knob2}, {52, 1, io::Control::Knob3},
    {54, 1, io::Control::Knob4}, {56, 1, io::Control::Knob5}, {58, 1, io::Control::Knob6},
    {60, 1, io::Control::Selector},
};

struct AnalogWord
{
    std::uint8_t offset;
    bool inverted; // 1023 − word = clockwise more
    io::Control control;
};
inline constexpr AnalogWord kAnalog[] = {
    {38, true, io::Control::AfsDrive}, {40, false, io::Control::MainVolume}, {42, true, io::Control::AfsHpCutoff}};
constexpr int kAnalogFullScale = 1023;

// LCD touch calibration (the stock firmware's default):
// screenX = round((rawLeft − rawX) · spanX / (rawLeft − rawRight)) + offsetX, Y likewise; contact = raw point inside
// the inclusive rectangle (idle reads (0, 0), always outside).
struct TouchCalibration
{
    int rawLeft = 940, rawRight = 72, rawTop = 87, rawBottom = 866;
    int spanX = 720, spanY = 405, offsetX = 40, offsetY = 29;
    int minX = 45, maxX = 967, minY = 45, maxY = 908;
};

// TX lamp channels: RGB channel 0x10 + k at bytes 1 + 3k, channel k at 0x31 + 3k; mono channels bit-packed.
constexpr int rgbOffset(int channel) noexcept
{
    return channel >= 0x10 ? 1 + 3 * (channel - 0x10) : 0x31 + 3 * channel;
}
struct MonoChannel
{
    std::uint8_t byte, bit, flagByte; // flagByte 0 = no dim flag; flag set = dim
};
inline constexpr MonoChannel kMono[35] = {
    {99, 4, 0x6a},  {99, 1, 0x6a},  {99, 2, 0x6a},  {99, 3, 0x6a},  {99, 7, 0},     {99, 6, 0x6a},  {99, 5, 0x6a},
    {100, 4, 0x6b}, {100, 6, 0x6b}, {100, 5, 0x6b}, {99, 0, 0x6a},  {100, 1, 0},    {100, 0, 0},    {100, 2, 0},
    {100, 3, 0},    {0x62, 0, 0},   {0x62, 1, 0},   {0x62, 2, 0},   {0x62, 3, 0},   {0x62, 4, 0},   {0x62, 5, 0},
    {0x62, 6, 0},   {0x62, 7, 0},   {0x65, 3, 0},   {0x65, 2, 0},   {0x65, 0, 0},   {0x61, 4, 0x69}, {0x61, 5, 0x69},
    {0x61, 6, 0x69}, {0x61, 7, 0x69}, {0x61, 0, 0x69}, {0x61, 1, 0x69}, {0x61, 2, 0x69}, {0x61, 3, 0x69}, {0x65, 1, 0},
};

struct LampRole
{
    io::Control control;
    bool rgb;
    std::uint8_t channel;
};
// pads, steps, mono keys and indicators, strip LEDs, BAR keys, bar position lamps
constexpr std::size_t kLampCount = 16 + 16 + 16 + 8 + 4 + 4;
inline constexpr auto kLamps = [] {
    std::array<LampRole, kLampCount> t {};
    std::size_t k = 0;
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::pad(i), true, static_cast<std::uint8_t>(16 + i - 1)};
    for (int i = 1; i <= 16; ++i)
        t[k++] = {io::step(i), true, static_cast<std::uint8_t>(i - 1)};
    const LampRole mono[] = {
        {io::Control::PadModeTrack, false, 0}, {io::Control::PadModeMute, false, 1},
        {io::Control::PadModeSlice, false, 2}, {io::Control::PadModeScale, false, 3},
        {io::Control::Rec, false, 5},          {io::Control::Stop, false, 6},
        {io::Control::Play, false, 7},         {io::Control::Scene, false, 8},
        {io::Control::Pattern, false, 9},      {io::Control::StripHold, false, 10},
        {io::Control::StripModeRepeat, false, 11}, {io::Control::StripModePitch, false, 12},
        {io::Control::StripModeUser1, false, 13},  {io::Control::StripModeUser2, false, 14},
        {io::Control::AfsActive, false, 23},   {io::Control::AfsOver, false, 34},
    };
    for (const auto& m : mono)
        t[k++] = m;
    for (int i = 0; i < 8; ++i)
        t[k++] = {static_cast<io::Control>(static_cast<int>(io::Control::StripLed1) + i), false,
                  static_cast<std::uint8_t>(15 + i)};
    const io::Control bars[] = {io::Control::Bar1, io::Control::Bar2, io::Control::Bar3, io::Control::Bar4};
    for (int i = 0; i < 4; ++i)
        t[k++] = {bars[i], false, static_cast<std::uint8_t>(26 + i)};
    for (int i = 1; i <= 4; ++i)
        t[k++] = {io::barPositionLamp(i), false, static_cast<std::uint8_t>(30 + i - 1)};
    return t;
}();
} // namespace platform::imx6::panel
