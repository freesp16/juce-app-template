// The panel link's frames (SP-16 or DJS-1000), device-independent: the 128-byte RX frame → logical ControlEvents
// (buttons by role, pads through the velocity curve, encoders and selector, analog pots, LCD touch) and the 128-byte
// TX lamp frame.
// The byte layout of each unit is in PanelRoles.h (PanelMap).
//
// Cost (aim: ≤ 1 % of a core at the panel frame rate): decode() touches 128 bytes, walks only the changed button
// bits, and never allocates.
#pragma once

#include "Platform.h"
#include "controls/PadCurve.h"
#include "imx6/PanelRoles.h"

#include <array>
#include <cstdint>

namespace platform::imx6::panel
{
std::uint16_t crcX25(const std::uint8_t* data, int n) noexcept;
bool rxFrameOk(const std::uint8_t* frame) noexcept; // header 0x01 and CRC

// Events of one frame (fixed capacity; a frame can change at most every button, pad, counter, pot and the touch).
struct EventBatch
{
    static constexpr int kCapacity = 96;
    std::array<io::ControlEvent, kCapacity> events {};
    int count = 0;
    void push(const io::ControlEvent& e) noexcept
    {
        if (count < kCapacity)
            events[static_cast<std::size_t>(count++)] = e;
    }
};

struct DecoderConfig
{
    platform::Unit unit = platform::Unit::Sp16; // whose panel (PanelRoles.h; its PanelMap::pads suit `pads`)
    io::PadCalibration pads {};
    io::PadResponse response {};
    TouchCalibration touch {};
    // A pot reports a change of ≥ 4 LSB (a unit's pots toggle by exactly 2 LSB at rest: 181↔183, 0↔2, which
    // flooded the queue with ~1000 events per pot), and readings within analogEndZone of a stop read as the stop
    // (reported once). analogEndZone must stay ≤ analogDeadband − 2.
    int analogDeadband = 4;
    int analogEndZone = 2;
    int touchStableFrames = 3; // frames in the contact rectangle before a touch is down (stock firmware: 4 samples)
    int touchJumpMax = 48;     // max raw units moved between those frames
    int touchReleaseFrames = 2;
};

// RX frame → events, stateful (counters are cumulative, buttons and pads report levels). Reader thread only.
class PanelDecoder
{
public:
    explicit PanelDecoder(const DecoderConfig& = {});
    void configure(const DecoderConfig&);
    // The pad calibration and response only (PAD CURVE / THRESHOLD / SENSITIVITY settings); rebuilds the velocity
    // tables, so call it between frames, not per frame.
    void setPads(const io::PadCalibration&, const io::PadResponse&);
    const DecoderConfig& config() const noexcept { return config_; }
    // Pad hits also go to `triggers` (the low-latency path) before they are added to the batch; may be null.
    void setTriggers(platform::TriggerSink* triggers) noexcept { triggers_ = triggers; }
    // `frame` must have passed rxFrameOk(). The first frame is the reference for the encoder counters; buttons, pads
    // and pots held at the first frame are reported (against all-up / unknown).
    void decode(const std::uint8_t* frame, double timeMs, EventBatch& out) noexcept;
    void reset() noexcept;

    // LCD touch raw point → logical 800 × 480 point (exposed for tests).
    static void touchToScreen(const TouchCalibration&, int rawX, int rawY, int& x, int& y) noexcept;

private:
    void decodeTouch(const std::uint8_t* f, double timeMs, EventBatch& out) noexcept;

    DecoderConfig config_;
    const PanelMap* map_ = &panelMap(platform::Unit::Sp16);
    io::PadCurve curve_;
    platform::TriggerSink* triggers_ = nullptr;
    bool haveFrame_ = false;
    std::array<std::uint16_t, 7> counters_ {};
    std::array<std::uint8_t, 7> buttons_ {};
    std::array<bool, 16> padDown_ {};
    std::array<std::uint8_t, 16> padPressure_ {};
    std::array<int, 3> analog_ {-1, -1, -1};
    // touch
    bool touchDown_ = false;
    int touchCandidate_ = 0, touchOut_ = 0, lastRawX_ = 0, lastRawY_ = 0;
    std::int16_t touchX_ = 0, touchY_ = 0;
};

// TX lamp frame: header 2, RGB channels, the other lamps (SP-16: one bit and a dim flag; DJS-1000: two bits each),
// brightness level, the SP-16's marker 0xaa at 0x77 or the DJS-1000's flags at 0x6c, CRC-16/X-25 at 0x7c (as RX).
class TxFrame
{
public:
    // The DJS-1000's flags byte (PanelMap::txFlagsOffset): the panel keeps the outputs muted while kFlagMute is set.
    static constexpr std::uint8_t kFlagMute = 0x01, kFlagStandby = 0x02, kFlagStartup = 0x04;

    explicit TxFrame(platform::Unit unit = platform::Unit::Sp16) : map_(&panelMap(unit)) { clear(); }
    void clear() noexcept;
    void setRgb(int channel, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept; // 0…kRgbFullScale each
    void setMono(int channel, bool lit, bool dim) noexcept;
    // A logical lamp (LampSink::setLamp semantics: rgb colour, level 0 = off … 255 = full). Mono lamps light for any
    // non-zero colour and level; level < kMonoDimBelow sets the dim flag (or the dim level) where the lamp has one.
    bool setLamp(io::Control, std::uint32_t rgb, std::uint8_t level) noexcept; // false: the panel has no such lamp
    // brightness, marker or flags, CRC; `flags` goes to the flags byte where the unit has one
    void finish(std::uint8_t flags = kFlagMute) noexcept;
    const std::uint8_t* data() const noexcept { return bytes_.data(); }
    bool operator==(const TxFrame& o) const noexcept { return bytes_ == o.bytes_; }
    platform::Unit unit() const noexcept { return map_->unit; }

    static constexpr std::uint8_t kMonoDimBelow = 160;

private:
    const PanelMap* map_;
    std::array<std::uint8_t, kFrameBytes> bytes_ {};
};
} // namespace platform::imx6::panel
