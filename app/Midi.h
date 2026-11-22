// MIDI in and out through ALSA raw MIDI (hw/common/MidiPort.h): the unit's DIN MIDI and USB MIDI ports, or any raw
// MIDI device on a desktop.
//
//   in    every input port is opened, each on its own reader thread; note on/off and pitch bend go straight to the
//         synth (Synth's queue), every message also to the UI's monitor (a lock-free queue drained by poll())
//   out   send() writes one message to every open output port (UI thread)
//   hot-plug  refresh() (UI thread, once a second) opens new ports and drops vanished ones — on the unit the USB port
//         disappears while the unit is in USB storage mode
#pragma once

#include "app/Synth.h"

#include "common/MidiPort.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace app
{
struct MidiMessageIn
{
    std::uint8_t bytes[3];
    std::uint8_t size;
    std::uint8_t port; // index into ports()
};

class Midi
{
public:
    explicit Midi(Synth& synth);
    ~Midi();

    void refresh(std::string* log = nullptr);           // UI thread: hot-plug pass; `log` gets "MIDI …" lines
    void poll(std::vector<MidiMessageIn>& out);         // UI thread: messages since the last call
    void send(const std::uint8_t* bytes, int size);     // UI thread: to every output
    void stop();

    struct PortState
    {
        std::string name;
        bool input = false, output = false;
    };
    const std::vector<PortState>& ports() const noexcept { return ports_; }
    int inputCount() const noexcept;
    int outputCount() const noexcept;

private:
    struct Input
    {
        std::string key;
        std::uint8_t index = 0;
        std::unique_ptr<io::midi::MidiInput> port;
        std::thread thread;
        std::atomic<bool> gone {false};
    };
    struct Output
    {
        std::string key;
        std::unique_ptr<io::midi::MidiOutput> port;
    };
    void read(Input& in);
    std::uint8_t indexOf(const std::string& name, bool input, bool output);

    Synth& synth_;
    std::vector<std::unique_ptr<io::midi::PortProvider>> providers_;
    std::vector<std::unique_ptr<Input>> inputs_;
    std::vector<Output> outputs_;
    std::vector<PortState> ports_;
    MpscQueue<MidiMessageIn, 1024> toUi_; // reader threads → UI
    std::atomic<bool> quit_ {false};
};
} // namespace app
