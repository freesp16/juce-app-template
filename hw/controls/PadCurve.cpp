// Pad velocity and pressure tables. See PadCurve.h for the calibration and the curve shapes.
#include "controls/PadCurve.h"

#include <algorithm>
#include <cmath>

namespace io
{
void PadCurve::configure(const PadCalibration& c, const PadResponse& r)
{
    calibration_ = c;
    const int threshold = std::clamp<int>(c.threshold + (std::clamp(r.threshold, 1, 16) - 1) * 8, 1, kWordMax);
    release_ = std::max(1, threshold / 2);
    const double span = std::max(1, static_cast<int>(c.ceiling) - static_cast<int>(c.floor));
    const double gain = std::clamp(r.sensitivity, 1, 16) / 8.0;
    const int n = static_cast<int>(r.shape);
    const double exponent = r.shape == PadCurveShape::Fixed ? 1.0 : (n < 0 ? 1.0 - n * 0.5 : 1.0 / (1.0 + n * 0.5));
    for (int w = 0; w <= kWordMax; ++w)
    {
        const auto i = static_cast<std::size_t>(w);
        if (w < threshold)
        {
            velocity_[i] = 0;
            pressure_[i] = 0;
            continue;
        }
        // Linear velocity, floor → 1, ceiling → 127 at neutral sensitivity.
        const double above = std::max(0.0, (w - static_cast<double>(c.floor)) * gain);
        const double x = std::min(1.0, above / span); // 0…1
        int v;
        if (r.shape == PadCurveShape::Fixed)
            v = std::clamp(r.fixedVelocity, 1, 127);
        else if (r.shape == PadCurveShape::Linear)
            v = 1 + static_cast<int>(std::lround(x * 126.0));
        else
            v = 1 + static_cast<int>(std::lround(std::pow(x, exponent) * 126.0));
        velocity_[i] = static_cast<std::uint8_t>(std::clamp(v, 1, 127));
        const double p = (w - threshold) / std::max(1.0, static_cast<double>(c.ceiling) - threshold);
        pressure_[i] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(p * 127.0)), 0, 127));
    }
}
} // namespace io
