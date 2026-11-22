// The audio: a 16-voice synth played by the pads, MIDI and a 16-step sequencer, plus a sine test tone that can
// be sent to any output jack. It is the platform's platform::AudioProcessor (the audio device calls process() once per
// period on its real-time thread) and its platform::TriggerSink (the panel's reader thread pushes pad hits and the
// strip's reader thread its moves straight here, so a pad sounds one panel frame after it is hit).
//
//   pads          note on (velocity) from the reader thread; note off from the UI thread (padRelease)
//   pad pressure  vibrato on that voice (up to ±½ semitone)
//   strip         pitch bend ±2 semitones while touched
//   MIDI in       note on / off, pitch bend (Midi's reader threads)
//   sequencer     16 steps of 1/16 at `bpm`, playing `seqNote`
//   test tone     a −12 dBFS sine on one port (MAIN, OUT 3-4, 5-6, 7-8, PHONES), left, right or both
// The synth mix (mono) goes to MAIN and PHONES.
//
// Threads: parameters are atomics the UI writes and process() reads once per period; events cross through one
// lock-free MPSC queue; the meters, the scope ring and the load figures are atomics the UI reads once per frame.
// process() never allocates, locks or does I/O. Cortex-A9 budget: ~3 % of one core with 16 voices sounding (sine table
// with linear interpolation, polyBLEP saw/square, one TPT state-variable filter on the mix, control rate every 32
// samples).
#pragma once

#include "Platform.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace app
{
// Bounded lock-free queue, many producers, one consumer (D. Vyukov's MPMC design): reader threads and the UI push,
// the audio thread pops. Never blocks or allocates; push() returns false when full. T must be trivially copyable.
template <typename T, std::size_t Capacity>
class MpscQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "items cross threads by copy");

public:
    MpscQueue() noexcept
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(const T& item) noexcept
    {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = cells_[pos & (Capacity - 1)];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0)
            {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.item = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
                return false; // full
            else
                pos = tail_.load(std::memory_order_relaxed);
        }
    }

    bool pop(T& out) noexcept // the single consumer
    {
        Cell& cell = cells_[head_ & (Capacity - 1)];
        const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
        if (static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(head_ + 1) < 0)
            return false; // empty
        out = cell.item;
        cell.sequence.store(head_ + Capacity, std::memory_order_release);
        ++head_;
        return true;
    }

private:
    struct Cell
    {
        std::atomic<std::size_t> sequence;
        T item;
    };
    Cell cells_[Capacity];
    alignas(64) std::atomic<std::size_t> tail_ {0};
    alignas(64) std::size_t head_ = 0;
};

constexpr int kOutPorts = platform::kAudioPorts;
const char* portName(int port) noexcept; // "MAIN", "OUT 3-4", …

class Synth final : public platform::AudioProcessor, public platform::TriggerSink
{
public:
    static constexpr int kVoices = 16;
    static constexpr int kScopeSize = 2048; // power of two

    Synth();

    // platform::AudioProcessor
    void prepare(double sampleRate, int maxDeviceFrames) override;
    void process(const platform::AudioIO&) noexcept override;
    // platform::TriggerSink (reader threads): pad hits, strip moves, pad pressure
    bool push(const platform::TriggerNow&) noexcept override;

    // Any thread (UI, MIDI readers). False when the event queue is full.
    bool noteOn(int note, int velocity) noexcept;
    bool noteOff(int note) noexcept;
    bool padRelease(int pad0) noexcept;
    bool pitchBend(float semitones) noexcept;
    bool allNotesOff() noexcept;

    // Parameters: written by the UI thread, read by process() once per period.
    std::atomic<float> volume {0.6f};    // 0…1 (cubic law)
    std::atomic<float> wave {0.0f};      // 0 sine … 1 triangle … 2 saw … 3 square (morphs in between)
    std::atomic<float> attackMs {4.0f};  // 1…2000
    std::atomic<float> releaseMs {250.0f};
    std::atomic<float> cutoff {1.0f};    // 0…1 → 40 Hz … 20 kHz (exponential)
    std::atomic<float> toneHz {440.0f};
    std::atomic<bool> toneOn {false};
    std::atomic<int> tonePort {0};       // platform::OutPort
    std::atomic<int> toneSide {0};       // 0 both, 1 left, 2 right
    std::atomic<bool> seqRunning {false};
    std::atomic<std::uint32_t> seqSteps {0x5555u}; // bit n = step n + 1
    std::atomic<int> seqNote {48};
    std::atomic<float> bpm {120.0f};
    std::array<std::atomic<std::uint8_t>, 16> padNote; // pad 0–15 → MIDI note

    // Feeds for the UI (written by process()).
    float takePeak(int port, int channel) noexcept { return peaks_[static_cast<std::size_t>(port * 2 + channel)].exchange(0.0f); }
    int step() const noexcept { return step_.load(std::memory_order_relaxed); }    // sequencer step 0–15, −1 = stopped
    int activeVoices() const noexcept { return voicesOn_.load(std::memory_order_relaxed); }
    float loadAverage() const noexcept { return loadAvg_.load(std::memory_order_relaxed); } // share of the period, 0…1
    float takeLoadMax() noexcept { return loadMax_.exchange(0.0f); }
    std::uint64_t periods() const noexcept { return periods_.load(std::memory_order_relaxed); }
    std::uint64_t droppedEvents() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    bool portPresent(int port) const noexcept { return present_[static_cast<std::size_t>(port)].load(std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_; }
    // `n` recent samples of everything the demo plays (synth + test tone), oldest first, starting at a rising zero
    // crossing when there is one (n ≤ kScopeSize / 2). Reads race with the writer: a frame may mix two periods, which a
    // scope shows as nothing worse than a seam.
    void scope(float* out, int n) const noexcept;

private:
    struct Event
    {
        enum Kind : std::uint8_t { NoteOn, NoteOff, PadOn, PadOff, Pressure, Bend, Strip, AllOff } kind;
        std::uint8_t a, b;
        float value;
    };
    struct Voice
    {
        float phase = 0, increment = 0, level = 0, gain = 0, pressure = 0;
        std::int8_t note = -1, pad = -1;
        bool held = false;
        std::uint32_t age = 0;
    };

    bool post(const Event& e) noexcept;
    void handle(const Event& e) noexcept;
    void start(int note, int velocity, int pad) noexcept;
    void release(int note, int pad) noexcept;
    void controlRate(float blockSeconds) noexcept;
    void render(float* mix, int frames) noexcept;

    MpscQueue<Event, 512> events_;
    std::array<Voice, kVoices> voices_ {};
    std::uint32_t ageCounter_ = 0;
    double sampleRate_ = 44100.0;
    float bendTarget_ = 0, bend_ = 0, lfoPhase_ = 0;
    float attackStep_ = 0, releaseCoef_ = 0, cutoffHz_ = 20000.0f, svfIc1_ = 0, svfIc2_ = 0;
    float tonePhase_ = 0, svfA1_ = 1, svfA2_ = 0, svfA3_ = 0;
    std::array<float, kVoices> voiceMod_ {};
    // sequencer
    double seqCounter_ = 0;
    int seqStep_ = -1, seqGate_ = 0, seqPlaying_ = -1;

    std::array<std::atomic<float>, kOutPorts * 2> peaks_ {};
    std::array<std::atomic<bool>, kOutPorts> present_ {};
    std::array<std::atomic<float>, kScopeSize> scope_ {};
    std::atomic<std::uint32_t> scopeWrite_ {0};
    std::atomic<int> step_ {-1}, voicesOn_ {0};
    std::atomic<float> loadAvg_ {0}, loadMax_ {0};
    std::atomic<std::uint64_t> periods_ {0}, dropped_ {0};
};
} // namespace app
