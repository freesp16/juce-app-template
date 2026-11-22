// Configuration ioctls on the SP-16's SPI input drivers (panel /dev/subucom_spi0.1, touch strip /dev/cdc_spi2.0).
// Device only. The drivers keep one transfer timer per device, not per fd, and refuse bits/bytes with EBUSY while it
// runs; the stock app leaves it on when it exits. So start() turns it off before configuring and on last, and stop()
// turns it off again before close() so the device is idle for whoever opens it next.
#pragma once

#include <sys/ioctl.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace platform::imx6
{
constexpr unsigned long reqSetTimer = 0x40017001UL; // u8: 0 = off, 1 = on (always allowed)

// One configuration ioctl; on failure `info` names the device, the step, the request code and the errno, e.g.
// "panel: set bits (0x40027003) failed: Device or resource busy".
inline bool driverIoctl(int fd, const char* dev, const char* step, unsigned long req, void* arg, std::string& info)
{
    if (::ioctl(fd, req, arg) == 0)
        return true;
    const int err = errno;
    char text[160];
    std::snprintf(text, sizeof text, "%s: %s (0x%08lx) failed: %s", dev, step, req, std::strerror(err));
    info = text;
    return false;
}

// Stops the device's transfer timer; best effort (used before configuring and before close()).
inline void driverTimerOff(int fd) noexcept
{
    std::uint8_t off = 0;
    (void) ::ioctl(fd, reqSetTimer, &off);
}
} // namespace platform::imx6
