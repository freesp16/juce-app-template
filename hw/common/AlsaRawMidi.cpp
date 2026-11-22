// ALSA raw MIDI ports, for the SP-16 and the Linux desktop, through libasound (already needed for audio).
//
// The unit: the kernel's DIN MIDI driver (`snd-din-midi`, loaded by the stock start script with `midi_thru`) and the
// USB-B MIDI gadget, which is mutually exclusive with USB storage mode — so the USB port vanishes while the unit
// exports its storage and comes back after; the app's once-a-second hot-plug pass reopens it by name. Both appear
// as ALSA raw MIDI devices (hw:C,D,S). Whether `midi_thru` already echoes DIN IN to DIN OUT in hardware is unverified.
// The desktop: every raw MIDI device of the sound cards (USB interfaces, snd-virmidi).
//
// Port name = "<card name> <device>:<subdevice> <subdevice name>" (the reconnect key; the unit's own cards read "MIDI
// USB" / "MIDI DIN"); id = "hw:C,D,S".
#include "common/MidiPort.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace platform
{
namespace
{
class AlsaInput final : public io::midi::MidiInput
{
public:
    explicit AlsaInput(snd_rawmidi_t* h) : h_(h) {}
    ~AlsaInput() override { snd_rawmidi_close(h_); }
    int read(std::uint8_t* buf, int cap, int timeoutMs) override
    {
        pollfd fds[4];
        const int n = snd_rawmidi_poll_descriptors(h_, fds, 4);
        if (n <= 0)
            return -1;
        const int r = ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
        if (r < 0)
            return 0; // EINTR
        if (r == 0)
            return 0;
        unsigned short revents = 0;
        if (snd_rawmidi_poll_descriptors_revents(h_, fds, static_cast<unsigned>(n), &revents) < 0 ||
            (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1; // unplugged
        if ((revents & POLLIN) == 0)
            return 0;
        const ssize_t got = snd_rawmidi_read(h_, buf, static_cast<size_t>(cap));
        if (got == -EAGAIN)
            return 0;
        return got < 0 ? -1 : static_cast<int>(got);
    }

private:
    snd_rawmidi_t* h_;
};

class AlsaOutput final : public io::midi::MidiOutput
{
public:
    explicit AlsaOutput(snd_rawmidi_t* h) : h_(h) {}
    ~AlsaOutput() override { snd_rawmidi_close(h_); }
    bool write(const std::uint8_t* bytes, int size) override
    {
        const ssize_t r = snd_rawmidi_write(h_, bytes, static_cast<size_t>(size));
        return r == size;
    }

private:
    snd_rawmidi_t* h_;
};

void listDirection(snd_ctl_t* ctl, int card, int device, const std::string& cardName, snd_rawmidi_stream_t stream,
                   std::vector<io::midi::PortInfo>& out)
{
    snd_rawmidi_info_t* info = nullptr;
    snd_rawmidi_info_alloca(&info);
    snd_rawmidi_info_set_device(info, static_cast<unsigned>(device));
    snd_rawmidi_info_set_stream(info, stream);
    snd_rawmidi_info_set_subdevice(info, 0);
    if (snd_ctl_rawmidi_info(ctl, info) < 0)
        return;
    const unsigned subs = snd_rawmidi_info_get_subdevices_count(info);
    for (unsigned s = 0; s < subs && s < 16; ++s)
    {
        snd_rawmidi_info_set_subdevice(info, s);
        if (snd_ctl_rawmidi_info(ctl, info) < 0)
            continue;
        char id[48];
        std::snprintf(id, sizeof id, "hw:%d,%d,%u", card, device, s);
        // The unit's two ports by what they are, not by their kernel drivers: the USB-B MIDI gadget
        // (card "g_midi") = MIDI USB, the DIN sockets (card "snd-din-midi") = MIDI DIN; a further device / subdevice
        // of the same card gets its number appended.
        const char* friendly = cardName == "g_midi" ? "MIDI USB" : cardName == "snd-din-midi" ? "MIDI DIN" : nullptr;
        std::string name;
        if (friendly != nullptr)
        {
            name = friendly;
            if (device > 0 || s > 0)
                name += " " + std::to_string(device * 16 + static_cast<int>(s) + 1);
        }
        else
        {
            const char* sub = snd_rawmidi_info_get_subdevice_name(info);
            name = cardName + " " + std::to_string(device) + ":" + std::to_string(s);
            if (sub != nullptr && *sub != '\0')
                name += std::string(" ") + sub;
        }
        bool merged = false;
        for (auto& p : out)
            if (p.id == id)
            {
                (stream == SND_RAWMIDI_STREAM_INPUT ? p.input : p.output) = true;
                merged = true;
            }
        if (! merged)
        {
            io::midi::PortInfo p {id, name, stream == SND_RAWMIDI_STREAM_INPUT, stream == SND_RAWMIDI_STREAM_OUTPUT};
            out.push_back(p);
        }
    }
}

class AlsaRawMidiProvider final : public io::midi::PortProvider
{
public:
    std::string_view id() const override { return "alsa-rawmidi"; }
    std::vector<io::midi::PortInfo> list() override
    {
        std::vector<io::midi::PortInfo> out;
        int card = -1;
        while (snd_card_next(&card) >= 0 && card >= 0)
        {
            char hw[16];
            std::snprintf(hw, sizeof hw, "hw:%d", card);
            snd_ctl_t* ctl = nullptr;
            if (snd_ctl_open(&ctl, hw, SND_CTL_NONBLOCK) < 0)
                continue;
            char* cardName = nullptr;
            std::string name = snd_card_get_name(card, &cardName) >= 0 && cardName != nullptr ? cardName : hw;
            std::free(cardName);
            int device = -1;
            while (snd_ctl_rawmidi_next_device(ctl, &device) >= 0 && device >= 0)
            {
                listDirection(ctl, card, device, name, SND_RAWMIDI_STREAM_INPUT, out);
                listDirection(ctl, card, device, name, SND_RAWMIDI_STREAM_OUTPUT, out);
            }
            snd_ctl_close(ctl);
        }
        return out;
    }
    std::unique_ptr<io::midi::MidiInput> openInput(const io::midi::PortInfo& p, std::string& error) override
    {
        snd_rawmidi_t* h = nullptr;
        const int r = snd_rawmidi_open(&h, nullptr, p.id.c_str(), SND_RAWMIDI_NONBLOCK);
        if (r < 0)
        {
            error = snd_strerror(r);
            return nullptr;
        }
        return std::make_unique<AlsaInput>(h);
    }
    std::unique_ptr<io::midi::MidiOutput> openOutput(const io::midi::PortInfo& p, std::string& error) override
    {
        snd_rawmidi_t* h = nullptr;
        // Opened non-blocking: a blocking open waits for as long as another process holds the port (the app would
        // hang at start-up). Then blocking writes: a message fits the buffer.
        const int r = snd_rawmidi_open(nullptr, &h, p.id.c_str(), SND_RAWMIDI_NONBLOCK);
        if (r < 0)
        {
            error = snd_strerror(r);
            return nullptr;
        }
        snd_rawmidi_nonblock(h, 0);
        return std::make_unique<AlsaOutput>(h);
    }
};

} // namespace
} // namespace platform

std::unique_ptr<io::midi::PortProvider> io::midi::makeAlsaRawMidi() { return std::make_unique<platform::AlsaRawMidiProvider>(); }
