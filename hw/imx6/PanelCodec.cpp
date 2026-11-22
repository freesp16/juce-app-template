// The SP-16 panel link's frames (see PanelCodec.h). Device-independent: also compiles on a desktop.
#include "imx6/PanelCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace platform::imx6::panel
{
namespace
{
inline std::uint16_t be16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

constexpr auto crcTable = [] {
    std::array<std::uint16_t, 256> t {};
    for (int b = 0; b < 256; ++b)
    {
        std::uint16_t crc = static_cast<std::uint16_t>(b);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408) : static_cast<std::uint16_t>(crc >> 1);
        t[static_cast<std::size_t>(b)] = crc;
    }
    return t;
}();

// Logical lamp → index into kLamps, -1 = no lamp on this panel.
constexpr auto lampIndex = [] {
    std::array<std::int8_t, io::kLampSlots> t {};
    t.fill(-1);
    for (std::size_t i = 0; i < kLamps.size(); ++i)
        t[static_cast<std::size_t>(io::lampSlot(kLamps[i].control))] = static_cast<std::int8_t>(i);
    return t;
}();
} // namespace

std::uint16_t crcX25(const std::uint8_t* data, int n) noexcept
{
    std::uint16_t crc = 0xffff;
    for (int i = 0; i < n; ++i)
        crc = static_cast<std::uint16_t>((crc >> 8) ^ crcTable[(crc ^ data[i]) & 0xff]);
    return static_cast<std::uint16_t>(crc ^ 0xffff);
}

bool rxFrameOk(const std::uint8_t* f) noexcept
{
    const auto stored = static_cast<std::uint16_t>(f[kCrcOffset] | f[kCrcOffset + 1] << 8);
    return f[0] == kRxHeader && crcX25(f, kCrcOffset) == stored;
}

// ---- RX ----

PanelDecoder::PanelDecoder(const DecoderConfig& c) { configure(c); }

void PanelDecoder::configure(const DecoderConfig& c)
{
    config_ = c;
    curve_.configure(c.pads, c.response);
}

void PanelDecoder::setPads(const io::PadCalibration& c, const io::PadResponse& r)
{
    config_.pads = c;
    config_.response = r;
    curve_.configure(c, r);
}

void PanelDecoder::reset() noexcept
{
    haveFrame_ = false;
    buttons_ = {};
    padDown_ = {};
    padPressure_ = {};
    analog_ = {-1, -1, -1};
    touchDown_ = false;
    touchCandidate_ = touchOut_ = 0;
}

void PanelDecoder::touchToScreen(const TouchCalibration& c, int rawX, int rawY, int& x, int& y) noexcept
{
    const double sx = (c.rawLeft - rawX) * static_cast<double>(c.spanX) / (c.rawLeft - c.rawRight) + c.offsetX;
    const double sy = (rawY - c.rawTop) * static_cast<double>(c.spanY) / (c.rawBottom - c.rawTop) + c.offsetY;
    x = std::clamp(static_cast<int>(std::lround(sx)), 0, 799);
    y = std::clamp(static_cast<int>(std::lround(sy)), 0, 479);
}

void PanelDecoder::decode(const std::uint8_t* f, double t, EventBatch& out) noexcept
{
    using Kind = io::ControlEvent::Kind;
    const auto src = io::kSourcePanel;

    // Pads first: their hits are the latency-critical part.
    const auto& cal = config_.pads;
    for (int i = 0; i < 16; ++i)
    {
        const int pressure = be16(f + kPadPressureOffset + 2 * i), contact = be16(f + kPadContactOffset + 2 * i);
        auto& down = padDown_[static_cast<std::size_t>(i)];
        if (! down)
        {
            const int velocity = contact >= cal.contactMin ? curve_.velocity(pressure) : 0;
            if (velocity == 0)
                continue;
            down = true;
            padPressure_[static_cast<std::size_t>(i)] = curve_.pressure(pressure);
            if (triggers_ != nullptr)
                triggers_->push({static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(velocity), 60,
                                 static_cast<std::int8_t>(i)});
            out.push({Kind::Press, io::pad(i + 1), static_cast<std::int16_t>(velocity), src, t});
        }
        else if (contact < cal.contactMin || pressure < curve_.releaseWord())
        {
            down = false;
            out.push({Kind::Release, io::pad(i + 1), 0, src, t});
        }
        else if (const auto p = curve_.pressure(pressure); p != padPressure_[static_cast<std::size_t>(i)])
        {
            padPressure_[static_cast<std::size_t>(i)] = p;
            out.push({Kind::Pressure, io::pad(i + 1), static_cast<std::int16_t>(p), src, t});
        }
    }

    // Buttons: only the changed bits.
    for (int b = 1; b <= 5; ++b)
    {
        unsigned changed = static_cast<unsigned>(f[b] ^ buttons_[static_cast<std::size_t>(b)]);
        while (changed != 0)
        {
            const int bit = __builtin_ctz(changed);
            changed &= changed - 1;
            const auto c = kButtonGrid.at[static_cast<std::size_t>(b)][static_cast<std::size_t>(bit)];
            if (c != io::Control::Count)
                out.push({(f[b] >> bit) & 1 ? Kind::Press : Kind::Release, c, 0, src, t});
        }
        buttons_[static_cast<std::size_t>(b)] = f[b];
    }

    // Encoders and selector: cumulative int16 counters, the first frame is the reference.
    for (std::size_t i = 0; i < std::size(kCounters); ++i)
    {
        const auto& k = kCounters[i];
        const auto v = be16(f + k.offset);
        if (haveFrame_)
            if (const auto d = static_cast<std::int16_t>(static_cast<std::uint16_t>(v - counters_[i])); d != 0)
                out.push({Kind::Turn, k.control, static_cast<std::int16_t>(d * k.sign), src, t});
        counters_[i] = v;
    }

    // Analog pots: absolute 0…1023, clockwise = more.
    for (std::size_t i = 0; i < std::size(kAnalog); ++i)
    {
        const auto& a = kAnalog[i];
        const int raw = std::min<int>(be16(f + a.offset), kAnalogFullScale);
        int v = a.inverted ? kAnalogFullScale - raw : raw;
        if (v <= config_.analogEndZone)
            v = 0;
        else if (v >= kAnalogFullScale - config_.analogEndZone)
            v = kAnalogFullScale;
        const int last = analog_[i];
        if (last < 0
            || (v != last && (std::abs(v - last) >= config_.analogDeadband || v == 0 || v == kAnalogFullScale)))
        {
            analog_[i] = v;
            out.push({Kind::Value, a.control, static_cast<std::int16_t>(v), src, t});
        }
    }

    decodeTouch(f, t, out);
    haveFrame_ = true;
}

// LCD touch: raw X/Y words, no contact bit; contact is a stable point inside the contact rectangle. Events:
// Kind::Touch on Control::Screen, value 1 = down, 2 = drag, 0 = up, x/y the logical 800 × 480 point.
void PanelDecoder::decodeTouch(const std::uint8_t* f, double t, EventBatch& out) noexcept
{
    const auto& c = config_.touch;
    const int rx = be16(f + kTouchXOffset), ry = be16(f + kTouchYOffset);
    const bool inside = rx >= c.minX && rx <= c.maxX && ry >= c.minY && ry <= c.maxY;
    const int jump = std::max(std::abs(rx - lastRawX_), std::abs(ry - lastRawY_));
    const bool steady = jump <= config_.touchJumpMax;
    lastRawX_ = rx;
    lastRawY_ = ry;
    auto touch = [&](std::int16_t phase) {
        io::ControlEvent e {io::ControlEvent::Kind::Touch, io::Control::Screen, phase, io::kSourcePanel, t};
        e.x = touchX_;
        e.y = touchY_;
        out.push(e);
    };
    if (! touchDown_)
    {
        touchCandidate_ = inside ? (touchCandidate_ > 0 && steady ? touchCandidate_ + 1 : 1) : 0;
        if (touchCandidate_ < config_.touchStableFrames)
            return;
        int x, y;
        touchToScreen(c, rx, ry, x, y);
        touchX_ = static_cast<std::int16_t>(x);
        touchY_ = static_cast<std::int16_t>(y);
        touchDown_ = true;
        touchOut_ = 0;
        touch(1);
        return;
    }
    if (! inside)
    {
        if (++touchOut_ >= config_.touchReleaseFrames)
        {
            touchDown_ = false;
            touchCandidate_ = 0;
            touch(0);
        }
        return;
    }
    touchOut_ = 0;
    int x, y;
    touchToScreen(c, rx, ry, x, y);
    if (x != touchX_ || y != touchY_)
    {
        touchX_ = static_cast<std::int16_t>(x);
        touchY_ = static_cast<std::int16_t>(y);
        touch(2);
    }
}

// ---- TX ----

void TxFrame::clear() noexcept
{
    bytes_.fill(0);
    bytes_[0] = kTxHeader;
}

void TxFrame::setRgb(int channel, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (channel < 0 || channel >= 32)
        return;
    auto* p = bytes_.data() + rgbOffset(channel);
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

void TxFrame::setMono(int channel, bool lit, bool dim) noexcept
{
    if (channel < 0 || channel >= static_cast<int>(std::size(kMono)))
        return;
    const auto& m = kMono[channel];
    const auto mask = static_cast<std::uint8_t>(1u << m.bit);
    bytes_[m.byte] = static_cast<std::uint8_t>(lit ? bytes_[m.byte] | mask : bytes_[m.byte] & ~mask);
    if (m.flagByte != 0)
        bytes_[m.flagByte] =
            static_cast<std::uint8_t>(lit && dim ? bytes_[m.flagByte] | mask : bytes_[m.flagByte] & ~mask);
}

bool TxFrame::setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) noexcept
{
    const int slot = io::lampSlot(c);
    if (slot < 0 || slot >= io::kLampSlots || lampIndex[static_cast<std::size_t>(slot)] < 0)
        return false;
    const auto& role = kLamps[static_cast<std::size_t>(lampIndex[static_cast<std::size_t>(slot)])];
    if (role.rgb)
    {
        // colour × level, scaled to the panel's full scale (0x4a): integer, rounded.
        auto chan = [&](int shift) {
            const unsigned v = (rgb >> shift) & 0xFF;
            const unsigned out = (v * level * kRgbFullScale + 255u * 255u / 2) / (255u * 255u);
            return static_cast<std::uint8_t>(out == 0 && v != 0 && level != 0 ? 1u : out); // a lit channel stays lit
        };
        setRgb(role.channel, chan(16), chan(8), chan(0));
    }
    else
        setMono(role.channel, level > 0 && (rgb & 0xFFFFFF) != 0, level < kMonoDimBelow);
    return true;
}

void TxFrame::finish() noexcept
{
    bytes_[0] = kTxHeader;
    bytes_[kTxBrightnessOffset] = 4; // NORMAL; the lamp levels carry the brightness preference
    bytes_[kTxMarkerOffset] = kTxMarker;
    const auto crc = crcX25(bytes_.data(), kCrcOffset);
    bytes_[kCrcOffset] = static_cast<std::uint8_t>(crc & 0xff);
    bytes_[kCrcOffset + 1] = static_cast<std::uint8_t>(crc >> 8);
}
} // namespace platform::imx6::panel
