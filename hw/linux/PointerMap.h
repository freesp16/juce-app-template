// Window pixels → the logical 800 × 480 point an LCD touch carries (ControlEvent::x/y): the linux window is
// 800·scale × 480·scale pixels with the canvas filling it exactly (same 5:3 aspect), so a point is pixel / scale,
// clamped to the screen (a drag may leave the window).
#pragma once

#include <cstdint>

namespace platform::linux_desktop
{
struct LogicalPoint
{
    std::int16_t x, y;
};

constexpr LogicalPoint toLogicalPoint(int px, int py, int scale) noexcept
{
    const int s = scale < 1 ? 1 : scale;
    auto clamp = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    // Negative pixels (outside the window) clamp to 0 before the division, so -1 does not round towards 0 by luck.
    return {static_cast<std::int16_t>(clamp(clamp(px, 800 * s - 1) / s, 799)),
            static_cast<std::int16_t>(clamp(clamp(py, 480 * s - 1) / s, 479))};
}
} // namespace platform::linux_desktop
