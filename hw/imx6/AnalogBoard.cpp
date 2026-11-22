#include "imx6/AnalogBoard.h"

#include "ProjectIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace platform::imx6::panel
{
namespace
{
constexpr const char* kGpioDevice = "/dev/gpiodrv";
constexpr const char* kDacDevice = "/dev/dac_spi4.0";

void analogLog(const char* what, const char* device, int err) noexcept
{
    std::printf("[sp16-mod] %s ANALOG %s %s%s%s\n", identity::envPrefix, what, device, err != 0 ? ": " : "",
                err != 0 ? std::strerror(err) : "");
    std::fflush(stdout);
}
} // namespace

AnalogBoard::~AnalogBoard()
{
    if (gpioFd_ >= 0)
        ::close(gpioFd_);
    if (dacFd_ >= 0)
        ::close(dacFd_);
}

unsigned AnalogBoard::dacWord(int value1023) noexcept
{
    const int v = std::clamp(value1023, 0, 1023);
    return kDacCommand | static_cast<unsigned>((v * 4095 + 511) / 1023);
}

void AnalogBoard::setActive(bool lit) noexcept
{
    if (! enabled_.load(std::memory_order_relaxed) || gpioFailed_ || lastActive_ == static_cast<int>(lit))
        return;
    if (gpioFd_ < 0)
    {
        gpioFd_ = ::open(kGpioDevice, O_RDWR | O_CLOEXEC);
        if (gpioFd_ < 0)
        {
            gpioFailed_ = true;
            analogLog("filter-select FAILED open", kGpioDevice, errno);
            return;
        }
    }
    const unsigned char level = lit ? 1 : 0;
    if (::lseek(gpioFd_, kSelectLine, SEEK_SET) != kSelectLine || ::write(gpioFd_, &level, 1) != 1)
    {
        gpioFailed_ = true;
        analogLog("filter-select FAILED write", kGpioDevice, errno);
        return;
    }
    if (lastActive_ < 0)
        analogLog("filter-select ok (GPIO5_IO05 follows ACTIVE)", kGpioDevice, 0);
    lastActive_ = lit ? 1 : 0;
}

void AnalogBoard::setHpCutoff(int value1023) noexcept
{
    if (! enabled_.load(std::memory_order_relaxed) || dacFailed_)
        return;
    const unsigned word = dacWord(value1023);
    if (lastCode_ == static_cast<int>(word))
        return;
    if (dacFd_ < 0)
    {
        dacFd_ = ::open(kDacDevice, O_WRONLY | O_CLOEXEC);
        if (dacFd_ < 0)
        {
            dacFailed_ = true;
            analogLog("hp-cutoff FAILED open", kDacDevice, errno);
            return;
        }
    }
    const unsigned char bytes[2] = {static_cast<unsigned char>(word & 0xFF), static_cast<unsigned char>(word >> 8)};
    if (::write(dacFd_, bytes, 2) != 2)
    {
        dacFailed_ = true;
        analogLog("hp-cutoff FAILED write", kDacDevice, errno);
        return;
    }
    if (lastCode_ < 0)
        analogLog("hp-cutoff ok (12-bit DAC word per HP CUTOFF change)", kDacDevice, 0);
    lastCode_ = static_cast<int>(word);
}
} // namespace platform::imx6::panel
