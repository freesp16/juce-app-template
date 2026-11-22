#include "app/Midi.h"

#include "app/Log.h"

#include <algorithm>

namespace app
{
Midi::Midi(Synth& synth) : synth_(synth)
{
    providers_.push_back(io::midi::makeAlsaRawMidi());
}

Midi::~Midi() { stop(); }

void Midi::stop()
{
    quit_ = true;
    for (auto& in : inputs_)
        if (in->thread.joinable())
            in->thread.join();
    inputs_.clear();
    outputs_.clear();
}

std::uint8_t Midi::indexOf(const std::string& name, bool input, bool output)
{
    for (std::size_t i = 0; i < ports_.size(); ++i)
        if (ports_[i].name == name)
        {
            ports_[i].input = ports_[i].input || input;
            ports_[i].output = ports_[i].output || output;
            return static_cast<std::uint8_t>(i);
        }
    ports_.push_back({name, input, output});
    return static_cast<std::uint8_t>(ports_.size() - 1);
}

int Midi::inputCount() const noexcept { return static_cast<int>(inputs_.size()); }
int Midi::outputCount() const noexcept { return static_cast<int>(outputs_.size()); }

void Midi::refresh(std::string* log)
{
    auto say = [&](const std::string& line) {
        if (log != nullptr)
            *log += line + "\n";
    };
    // Drop inputs whose reader saw the port vanish.
    for (auto it = inputs_.begin(); it != inputs_.end();)
    {
        if ((*it)->gone.load())
        {
            if ((*it)->thread.joinable())
                (*it)->thread.join();
            say("MIDI in closed: " + (*it)->key);
            ports_[(*it)->index].input = false;
            it = inputs_.erase(it);
        }
        else
            ++it;
    }
    for (auto& provider : providers_)
    {
        const auto list = provider->list();
        for (const auto& info : list)
        {
            const std::string key = std::string(provider->id()) + ":" + info.name;
            if (info.input && std::none_of(inputs_.begin(), inputs_.end(), [&](const auto& in) { return in->key == key; }))
            {
                std::string error;
                if (auto port = provider->openInput(info, error))
                {
                    auto in = std::make_unique<Input>();
                    in->key = key;
                    in->index = indexOf(info.name, true, false);
                    in->port = std::move(port);
                    Input* raw = in.get();
                    in->thread = std::thread([this, raw] { read(*raw); });
                    inputs_.push_back(std::move(in));
                    say("MIDI in opened: " + key + " (" + info.id + ")");
                }
                else
                    say("MIDI in FAILED: " + key + ": " + error);
            }
            if (info.output && std::none_of(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o.key == key; }))
            {
                std::string error;
                if (auto port = provider->openOutput(info, error))
                {
                    indexOf(info.name, false, true);
                    outputs_.push_back({key, std::move(port)});
                    say("MIDI out opened: " + key + " (" + info.id + ")");
                }
                else
                    say("MIDI out FAILED: " + key + ": " + error);
            }
        }
    }
}

// Reader thread: a running-status parser for channel messages; system exclusive is skipped, real-time bytes
// (clock, active sensing) are ignored.
void Midi::read(Input& in)
{
    nameThisThread("midi-in");
    std::uint8_t buffer[256];
    std::uint8_t status = 0, data[2] = {}, have = 0;
    bool sysex = false;
    while (! quit_.load(std::memory_order_relaxed))
    {
        const int n = in.port->read(buffer, sizeof buffer, 100);
        if (n < 0)
        {
            in.gone = true;
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            const std::uint8_t b = buffer[i];
            if (b >= 0xF8)
                continue; // real-time
            if (b & 0x80)
            {
                sysex = b == 0xF0;
                status = b < 0xF0 ? b : 0;
                have = 0;
                continue;
            }
            if (sysex || status == 0)
                continue;
            data[have++] = b;
            const std::uint8_t type = status & 0xF0;
            const std::uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (have < need)
                continue;
            have = 0;
            switch (type)
            {
            case 0x90: data[1] > 0 ? synth_.noteOn(data[0], data[1]) : synth_.noteOff(data[0]); break;
            case 0x80: synth_.noteOff(data[0]); break;
            case 0xE0: synth_.pitchBend(static_cast<float>((data[1] << 7 | data[0]) - 8192) / 8192.0f * 2.0f); break;
            case 0xB0:
                if (data[0] == 123 || data[0] == 120) // all notes off / all sound off
                    synth_.allNotesOff();
                break;
            default: break;
            }
            toUi_.push({{status, data[0], data[1]}, need == 1 ? std::uint8_t {2} : std::uint8_t {3}, in.index});
        }
    }
}

void Midi::poll(std::vector<MidiMessageIn>& out)
{
    for (MidiMessageIn m; toUi_.pop(m);)
        out.push_back(m);
}

void Midi::send(const std::uint8_t* bytes, int size)
{
    // A failed write = the port is gone (unplugged): drop it, the next refresh() reopens it when it is back.
    std::erase_if(outputs_, [&](Output& o) { return ! o.port->write(bytes, size); });
}
} // namespace app
