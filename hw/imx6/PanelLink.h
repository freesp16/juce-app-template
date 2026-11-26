// The hardware panel link (the SP-16 platform's "panel" io::ControlSource, and the sender of its lamp frames):
// /dev/subucom_spi0.1, 128-byte RX frames in, 128-byte TX lamp frames out.
//
// The panel driver accepts ioctls for word size, byte order, transfer size, interval and scan timer; a TX frame is
// sent with request 0x40107000 and a 16-byte SPI transfer descriptor (multi 0, part 0 of 1). The frame layout is
// PanelRoles.h, the decoding PanelCodec.cpp.
//
// Threads: one reader thread (normal priority, never the audio thread) polls the link and a wake pipe. Per RX frame it
// decodes the events, pushes pad hits straight into the app's TriggerSink and queues every event into an
// SPSC queue (reader → UI thread) that poll() drains; between frames it sends the TX frame the LampBus says is due
// (on change, heartbeat ≤ 4 Hz). Only one process may own the link: this replaces the stock application.
#pragma once

#include "common/SpscQueue.h"
#include "Platform.h"
#include "imx6/PanelCodec.h"
#include "imx6/PanelLamps.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace platform::imx6
{
struct PanelStats
{
    long frames = 0, badCrc = 0, shortReads = 0, txFrames = 0, txErrors = 0, dropped = 0;
    bool running = false;
};

class PanelLink final : public io::ControlSource
{
public:
    // `board` (may be null): every HP CUTOFF pot change also goes to the analog filter's DAC (AnalogBoard.h).
    // `unit`: whose panel (PanelRoles.h); `pads` its pad calibration.
    PanelLink(platform::Unit unit, const io::PadCalibration& pads, platform::TriggerSink* triggers,
              std::shared_ptr<panel::LampBus> lamps, bool trace,
              std::shared_ptr<panel::AnalogBoard> board = {});
    ~PanelLink() override;
    std::string_view id() const override { return "panel"; }
    bool start(std::string& info) override; // opens and configures the link, starts the reader thread
    void stop() override;
    void poll(std::vector<io::ControlEvent>& out) override; // UI thread, wait-free
    PanelStats stats() const;
    // DJS-1000: each PHONES LEVEL value, as a gain 0…1 (an audio taper, the square of the knob), goes here.
    void setPhonesLevel(std::shared_ptr<std::atomic<float>> level) { phonesLevel_ = std::move(level); }

private:
    void run();
    void sendDue(double nowMs, double& waitMs);
    static void wake(void* self);

    const panel::PanelMap& map_;
    platform::TriggerSink* triggers_;
    bool takenOver_ = false;
    std::uint8_t lastPowerByte_ = 0;
    std::shared_ptr<panel::LampBus> lamps_;
    std::shared_ptr<panel::AnalogBoard> board_;
    std::shared_ptr<std::atomic<float>> phonesLevel_;
    bool trace_;
    panel::PanelDecoder decoder_;
    panel::EventBatch batch_;
    platform::SpscQueue<io::ControlEvent, 1024> queue_;
    int fd_ = -1, wakeRead_ = -1, wakeWrite_ = -1;
    std::thread thread_;
    std::atomic<bool> quit_ {false}, running_ {false};
    std::atomic<long> frames_ {0}, badCrc_ {0}, shortReads_ {0}, txFrames_ {0}, txErrors_ {0}, dropped_ {0};
};
} // namespace platform::imx6
