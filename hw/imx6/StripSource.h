// The touch strip as the SP-16 platform's "cdc-strip" io::ControlSource: /dev/cdc_spi2.0, 4-byte frames on change
// (StripCodec.h). Driver ioctls: capacitive converter reset 0x40017006, transfer size 0x40047004, interval
// 0x40047002, scan timer 0x40017001 (the size must be set while the timer is off).
//
// A reader thread (normal priority) decodes frames into an SPSC queue that poll() drains on the UI thread; every
// event also goes to the panel's LampBus for the local strip-LED echo, and straight to the app's TriggerSink
// (platform::stripTrigger), so the audio hears a move one audio block later, not one UI frame later (the UI side
// does not send these again). stop() logs one "CDC-STRIP frames=… events=… dropped=… engine_dropped=…" line.
#pragma once

#include "common/SpscQueue.h"
#include "Platform.h"
#include "imx6/PanelLamps.h"
#include "imx6/StripCodec.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace platform::imx6
{
class StripSource final : public io::ControlSource
{
public:
    StripSource(std::shared_ptr<panel::LampBus> lamps, bool trace, platform::TriggerSink* engine = nullptr)
        : lamps_(std::move(lamps)), trace_(trace), engine_(engine)
    {
    }
    ~StripSource() override;
    std::string_view id() const override { return "cdc-strip"; }
    bool start(std::string& info) override;
    void stop() override;
    void poll(std::vector<io::ControlEvent>& out) override;

private:
    void run();

    std::shared_ptr<panel::LampBus> lamps_;
    bool trace_;
    platform::TriggerSink* engine_;
    strip::StripDecoder decoder_;
    platform::SpscQueue<io::ControlEvent, 256> queue_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> quit_ {false};
    std::atomic<long> frames_ {0}, dropped_ {0}, events_ {0}, engineDropped_ {0};
};
} // namespace platform::imx6
