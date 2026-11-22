// The analog filter board's two digital controls (/dev/gpiodrv and /dev/dac_spi4.0):
//
//   filter select  GPIO5_IO05 "SEL_xTHRU/DSF": the vendor GPIO driver, one byte per line at offset (bank − 1)·32 +
//                  number = 133, bit 0 = the level (seek, then write). The stock firmware drives it high while
//                  ANALOG FILTER ACTIVE is lit, so it follows the ACTIVE lamp here (PanelLamps::setLamp).
//   HP CUTOFF      the high-pass cutoff DAC: one 2-byte little-endian word per change of the HP CUTOFF pot, the 12-bit
//                  code in bits 0–11 (code 0 = the lowest cutoff, about 20 Hz, = the pot fully counter-clockwise =
//                  afs.hpCutoff 0) and the converter's command in bits 12–15 (0001 = load and update, assumed from a
//                  DAC7811-style part; not confirmed on hardware).
//
// LP CUTOFF and RESONANCE are analog pots on that board that no microcontroller reads: they act on the audio of the
// send/return loop only (AlsaOutputConfig::loopDevice, setting filter_loop=1). A device that does not open
// fails once, is reported in one log line and is not retried. Thread-safe by split: setActive() runs on the message
// thread, setHpCutoff() on the panel reader thread; each owns its own descriptor and state.
#pragma once

#include <atomic>

namespace platform::imx6::panel
{
class AnalogBoard
{
public:
    static constexpr int kSelectLine = 4 * 32 + 5; // GPIO5_IO05
    static constexpr unsigned kDacCommand = 0x1000; // bits 12–15 = 0001

    AnalogBoard() = default;
    ~AnalogBoard();
    AnalogBoard(const AnalogBoard&) = delete;
    AnalogBoard& operator=(const AnalogBoard&) = delete;

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); } // before the sources start
    void setActive(bool lit) noexcept;        // message thread: the filter-select line, on change only
    void setHpCutoff(int value1023) noexcept; // panel reader thread: the DAC word, on change only

    // The 2-byte DAC word of an afs.hpCutoff value 0–1023 (also for the tests).
    static unsigned dacWord(int value1023) noexcept;

private:
    std::atomic<bool> enabled_ {false}; // Imx6Platform::configure turns it on with the filter loop
    int gpioFd_ = -1, dacFd_ = -1;
    bool gpioFailed_ = false, dacFailed_ = false;
    int lastActive_ = -1, lastCode_ = -1;
};
} // namespace platform::imx6::panel
