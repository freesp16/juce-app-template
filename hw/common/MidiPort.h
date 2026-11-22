// MIDI ports behind an interface. A PortProvider enumerates and opens ports by name; the app (app/Midi.h) owns the
// threads, the hot-plug poll and the reopen-by-name, the hardware layer only supplies the provider:
//
//   "alsa-rawmidi"  common/AlsaRawMidi.cpp (the unit and the Linux desktop): ALSA raw MIDI through libasound — on
//                   the unit the DIN driver and the USB MIDI gadget (USB MIDI and USB storage are mutually exclusive
//                   gadget modes, so the USB port appears and disappears — hot-plug)
#pragma once


#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace io::midi
{
struct PortInfo
{
    std::string id;   // provider-specific address ("hw:1,0,0")
    std::string name; // stable display name, the reconnect key ("USB MIDI 1"); unique per provider
    bool input = false, output = false;
};

class MidiInput
{
public:
    virtual ~MidiInput() = default;
    // Blocks for at most timeoutMs; returns the bytes read (0 = timeout), −1 = the port is gone (unplugged).
    // Called from the port's own thread only.
    virtual int read(std::uint8_t* buffer, int capacity, int timeoutMs) = 0;
};

class MidiOutput
{
public:
    virtual ~MidiOutput() = default;
    // Writes one complete message (sender thread only); false = the port is gone.
    virtual bool write(const std::uint8_t* bytes, int size) = 0;
};

class PortProvider
{
public:
    virtual ~PortProvider() = default;
    virtual std::string_view id() const = 0;
    virtual std::vector<PortInfo> list() = 0; // message thread (hot-plug poll, every 1 s)
    virtual std::unique_ptr<MidiInput> openInput(const PortInfo&, std::string& error) = 0;
    virtual std::unique_ptr<MidiOutput> openOutput(const PortInfo&, std::string& error) = 0;
};

// ALSA raw MIDI (common/AlsaRawMidi.cpp): every raw MIDI device of the sound cards — on the unit "MIDI DIN" and
// "MIDI USB" (the USB-B gadget, which vanishes while the unit is in USB storage mode).
std::unique_ptr<PortProvider> makeAlsaRawMidi();
} // namespace io::midi
