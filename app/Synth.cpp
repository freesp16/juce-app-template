#include "app/Synth.h"

#include "app/Log.h"


#include <time.h>

#include <algorithm>
#include <cmath>

namespace app
{
namespace
{
constexpr int kBlock = 32;               // control-rate block (samples)
constexpr float kToneGain = 0.25119f;    // −12 dBFS
constexpr float kVoiceGain = 0.3f;       // headroom for several voices before the soft clip
constexpr float kTwoPi = 6.28318530718f;

// One sine period in 1024 steps (+1 guard point for the interpolation), built once.
struct SineTable
{
    float v[1025];
    SineTable()
    {
        for (int i = 0; i <= 1024; ++i)
            v[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * i / 1024.0));
    }
};
const SineTable sineTable;

inline float sine(float phase) noexcept // phase 0…1
{
    const float x = phase * 1024.0f;
    const int i = static_cast<int>(x);
    const float f = x - static_cast<float>(i);
    return sineTable.v[i] + f * (sineTable.v[i + 1] - sineTable.v[i]);
}

inline float polyBlep(float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float waveform(int shape, float phase, float dt) noexcept
{
    switch (shape)
    {
    case 0: return sine(phase);
    case 1: return 4.0f * std::fabs(phase - 0.5f) - 1.0f; // triangle
    case 2: return 2.0f * phase - 1.0f - polyBlep(phase, dt); // saw
    default:
    {
        float half = phase + 0.5f;
        half -= half >= 1.0f ? 1.0f : 0.0f;
        return (phase < 0.5f ? 1.0f : -1.0f) + polyBlep(phase, dt) - polyBlep(half, dt); // square
    }
    }
}

inline float softClip(float x) noexcept // rational tanh approximation, |out| ≤ 1
{
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

double monotonicSeconds() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec);
}

inline void raise(std::atomic<float>& peak, float value) noexcept
{
    if (value > peak.load(std::memory_order_relaxed))
        peak.store(value, std::memory_order_relaxed);
}
} // namespace

const char* portName(int port) noexcept
{
    static const char* const names[] = {"MAIN", "OUT 3-4", "OUT 5-6", "OUT 7-8", "PHONES", "FILTER SEND"};
    return port >= 0 && port < kOutPorts ? names[port] : "?";
}

Synth::Synth()
{
    static constexpr std::uint8_t minorPentatonic[] = {0, 3, 5, 7, 10};
    for (int i = 0; i < 16; ++i) // pad 1 = C3, upwards in C minor pentatonic
        padNote[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(48 + 12 * (i / 5) + minorPentatonic[i % 5]);
}

void Synth::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    for (auto& v : voices_)
        v = {};
    svfIc1_ = svfIc2_ = 0;
    seqStep_ = -1;
    seqPlaying_ = -1;
}

bool Synth::post(const Event& e) noexcept
{
    if (events_.push(e))
        return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool Synth::push(const platform::TriggerNow& t) noexcept
{
    if (t.pad == platform::kStripTriggerPad)
        return post({Event::Strip, t.velocity, t.note, 0.0f});
    if (t.pad == platform::kPressureTriggerPad)
        return post({Event::Pressure, t.track, 0, static_cast<float>(t.velocity) / 127.0f});
    if (t.pad >= 0 && t.pad < 16 && t.velocity > 0)
        return post({Event::PadOn, static_cast<std::uint8_t>(t.pad), t.velocity, 0.0f});
    return false;
}

bool Synth::noteOn(int note, int velocity) noexcept
{
    if (velocity <= 0)
        return noteOff(note);
    return post({Event::NoteOn, static_cast<std::uint8_t>(note & 127), static_cast<std::uint8_t>(std::min(velocity, 127)), 0.0f});
}
bool Synth::noteOff(int note) noexcept { return post({Event::NoteOff, static_cast<std::uint8_t>(note & 127), 0, 0.0f}); }
bool Synth::padRelease(int pad0) noexcept { return post({Event::PadOff, static_cast<std::uint8_t>(pad0 & 15), 0, 0.0f}); }
bool Synth::pitchBend(float semitones) noexcept { return post({Event::Bend, 0, 0, semitones}); }
bool Synth::allNotesOff() noexcept { return post({Event::AllOff, 0, 0, 0.0f}); }

void Synth::handle(const Event& e) noexcept
{
    switch (e.kind)
    {
    case Event::NoteOn: start(e.a, e.b, -1); break;
    case Event::NoteOff: release(e.a, -1); break;
    case Event::PadOn: start(padNote[e.a].load(std::memory_order_relaxed), e.b, e.a); break;
    case Event::PadOff: release(-1, e.a); break;
    case Event::Pressure:
        for (auto& v : voices_)
            if (v.pad == static_cast<std::int8_t>(e.a) && v.held)
                v.pressure = e.value;
        break;
    case Event::Bend: bendTarget_ = e.value; break;
    case Event::Strip: bendTarget_ = e.b != 0 ? (static_cast<float>(e.a) - 64.0f) / 64.0f * 2.0f : 0.0f; break;
    case Event::AllOff:
        for (auto& v : voices_)
            v.held = false;
        break;
    }
}

void Synth::start(int note, int velocity, int pad) noexcept
{
    // A free voice, else the same note (retrigger), else the oldest released one, else the oldest.
    Voice* best = nullptr;
    for (auto& v : voices_)
        if (v.note < 0 || (v.note == note && v.pad == pad))
        {
            best = &v;
            break;
        }
    if (best == nullptr)
        for (auto& v : voices_)
            if (! v.held && (best == nullptr || v.age < best->age))
                best = &v;
    if (best == nullptr)
        for (auto& v : voices_)
            if (best == nullptr || v.age < best->age)
                best = &v;
    const float vel = static_cast<float>(velocity) / 127.0f;
    best->note = static_cast<std::int8_t>(note);
    best->pad = static_cast<std::int8_t>(pad);
    best->held = true;
    best->gain = kVoiceGain * vel * std::sqrt(vel);
    best->pressure = 0;
    best->increment = static_cast<float>(440.0 * std::exp2((note - 69) / 12.0) / sampleRate_);
    best->age = ++ageCounter_;
}

void Synth::release(int note, int pad) noexcept
{
    for (auto& v : voices_)
        if (v.held && (pad >= 0 ? v.pad == pad : (v.note == note && v.pad == pad)))
            v.held = false;
}

void Synth::controlRate(float blockSeconds) noexcept
{
    const float attack = std::max(1.0f, attackMs.load(std::memory_order_relaxed));
    const float rel = std::max(5.0f, releaseMs.load(std::memory_order_relaxed));
    attackStep_ = 1000.0f / (attack * static_cast<float>(sampleRate_));
    releaseCoef_ = std::exp(-6.9f * 1000.0f / (rel * static_cast<float>(sampleRate_))); // −60 dB after `rel` ms

    // Filter: the cutoff glides (≈ 20 ms) to the target; TPT state-variable low-pass, Q ≈ 0.7.
    const float target = 40.0f * std::exp2(9.0f * std::clamp(cutoff.load(std::memory_order_relaxed), 0.0f, 1.0f));
    cutoffHz_ += (target - cutoffHz_) * std::min(1.0f, blockSeconds * 50.0f);
    const float fc = std::min(cutoffHz_, 0.45f * static_cast<float>(sampleRate_));
    const float g = std::tan(3.14159265f * fc / static_cast<float>(sampleRate_));
    constexpr float k = 1.41421356f;
    svfA1_ = 1.0f / (1.0f + g * (g + k));
    svfA2_ = g * svfA1_;
    svfA3_ = g * svfA2_;

    // Pitch: bend glides (≈ 10 ms); pressure = vibrato 5.5 Hz, up to ±½ semitone.
    bend_ += (bendTarget_ - bend_) * std::min(1.0f, blockSeconds * 100.0f);
    lfoPhase_ += 5.5f * blockSeconds;
    lfoPhase_ -= lfoPhase_ >= 1.0f ? 1.0f : 0.0f;
    const float lfo = sine(lfoPhase_);
    for (int i = 0; i < kVoices; ++i)
        voiceMod_[static_cast<std::size_t>(i)] = std::exp2((bend_ + 0.5f * voices_[static_cast<std::size_t>(i)].pressure * lfo) / 12.0f);
}

void Synth::render(float* mix, int frames) noexcept
{
    std::fill(mix, mix + frames, 0.0f);
    const float w = std::clamp(wave.load(std::memory_order_relaxed), 0.0f, 3.0f);
    const int shapeA = std::min(2, static_cast<int>(w));
    const float morph = w - static_cast<float>(shapeA);
    for (int i = 0; i < kVoices; ++i)
    {
        Voice& v = voices_[static_cast<std::size_t>(i)];
        if (v.note < 0)
            continue;
        const float inc = std::min(0.45f, v.increment * voiceMod_[static_cast<std::size_t>(i)]);
        float phase = v.phase, level = v.level;
        for (int n = 0; n < frames; ++n)
        {
            const float a = waveform(shapeA, phase, inc);
            const float s = morph > 0.0f ? a + morph * (waveform(shapeA + 1, phase, inc) - a) : a;
            if (v.held)
                level = std::min(1.0f, level + attackStep_);
            else
                level *= releaseCoef_;
            mix[n] += s * level * v.gain;
            phase += inc;
            phase -= phase >= 1.0f ? 1.0f : 0.0f;
        }
        v.phase = phase;
        v.level = level;
        if (! v.held && level < 1e-4f)
            v.note = -1, v.pad = -1;
    }
    // The mix through the low-pass.
    float ic1 = svfIc1_, ic2 = svfIc2_;
    for (int n = 0; n < frames; ++n)
    {
        const float v3 = mix[n] - ic2;
        const float v1 = svfA1_ * ic1 + svfA2_ * v3;
        const float v2 = ic2 + svfA2_ * ic1 + svfA3_ * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        mix[n] = v2;
    }
    svfIc1_ = std::fabs(ic1) < 1e-20f ? 0.0f : ic1; // no denormals in the state
    svfIc2_ = std::fabs(ic2) < 1e-20f ? 0.0f : ic2;
}

void Synth::process(const platform::AudioIO& io) noexcept
{
    if (periods_.load(std::memory_order_relaxed) == 0)
        nameThisThread("audio"); // once: the crash report names the thread
    const double t0 = monotonicSeconds();
    for (int p = 0; p < kOutPorts; ++p)
    {
        const bool present = p < io.numOutPorts && io.out[2 * p] != nullptr;
        present_[static_cast<std::size_t>(p)].store(present, std::memory_order_relaxed);
    }
    for (Event e; events_.pop(e);)
        handle(e);

    const float sr = static_cast<float>(sampleRate_);
    const float vol = volume.load(std::memory_order_relaxed);
    const float masterGain = vol * vol;
    const bool tone = toneOn.load(std::memory_order_relaxed);
    const int toneTo = std::clamp(tonePort.load(std::memory_order_relaxed), 0, kOutPorts - 1);
    const int side = toneSide.load(std::memory_order_relaxed);
    const float toneInc = std::clamp(toneHz.load(std::memory_order_relaxed), 20.0f, 20000.0f) / sr;
    const bool seqOn = seqRunning.load(std::memory_order_relaxed);
    const double samplesPerStep = sampleRate_ * 60.0 / (4.0 * static_cast<double>(std::clamp(bpm.load(std::memory_order_relaxed), 40.0f, 300.0f)));
    const std::uint32_t steps = seqSteps.load(std::memory_order_relaxed);
    std::uint32_t scopeAt = scopeWrite_.load(std::memory_order_relaxed);

    float block[kBlock];
    for (int done = 0; done < io.frames;)
    {
        const int n = std::min(kBlock, io.frames - done);
        // Sequencer, at block resolution (≤ 0.7 ms).
        if (seqOn)
        {
            if (seqStep_ < 0)
                seqCounter_ = samplesPerStep; // first step at once
            seqCounter_ += n;
            while (seqCounter_ >= samplesPerStep)
            {
                seqCounter_ -= samplesPerStep;
                seqStep_ = (seqStep_ + 1) & 15;
                if (steps & (1u << seqStep_))
                {
                    if (seqPlaying_ >= 0)
                        release(seqPlaying_, -2);
                    seqPlaying_ = seqNote.load(std::memory_order_relaxed) & 127;
                    start(seqPlaying_, 100, -2);
                    seqGate_ = static_cast<int>(samplesPerStep * 0.5);
                }
            }
        }
        else if (seqStep_ >= 0)
        {
            seqStep_ = -1;
            seqGate_ = 0;
        }
        if (seqPlaying_ >= 0 && (seqGate_ -= n) <= 0)
        {
            release(seqPlaying_, -2);
            seqPlaying_ = -1;
        }

        controlRate(static_cast<float>(n) / sr);
        render(block, n);

        float synthOut[kBlock], toneOut[kBlock];
        for (int i = 0; i < n; ++i)
            synthOut[i] = softClip(block[i] * masterGain);
        if (tone)
            for (int i = 0; i < n; ++i)
            {
                toneOut[i] = kToneGain * sine(tonePhase_);
                tonePhase_ += toneInc;
                tonePhase_ -= tonePhase_ >= 1.0f ? 1.0f : 0.0f;
            }
        for (int p = 0; p < kOutPorts; ++p)
        {
            if (p >= io.numOutPorts || io.out[2 * p] == nullptr)
                continue;
            float* l = io.out[2 * p] + done;
            float* r = io.out[2 * p + 1] != nullptr ? io.out[2 * p + 1] + done : nullptr;
            const bool synth = p == static_cast<int>(platform::AudioPort::Main) || p == static_cast<int>(platform::AudioPort::Phones);
            const bool toneL = tone && p == toneTo && side != 2, toneR = tone && p == toneTo && side != 1;
            float peakL = 0, peakR = 0;
            for (int i = 0; i < n; ++i)
            {
                const float s = synth ? synthOut[i] : 0.0f;
                l[i] = s + (toneL ? toneOut[i] : 0.0f);
                peakL = std::max(peakL, std::fabs(l[i]));
            }
            if (r != nullptr)
                for (int i = 0; i < n; ++i)
                {
                    r[i] = (synth ? synthOut[i] : 0.0f) + (toneR ? toneOut[i] : 0.0f);
                    peakR = std::max(peakR, std::fabs(r[i]));
                }
            raise(peaks_[static_cast<std::size_t>(2 * p)], peakL);
            raise(peaks_[static_cast<std::size_t>(2 * p + 1)], peakR);
        }
        for (int i = 0; i < n; ++i) // the scope: everything the demo plays, wherever it goes
            scope_[(scopeAt + static_cast<std::uint32_t>(i)) & (kScopeSize - 1)].store(synthOut[i] + (tone ? toneOut[i] : 0.0f),
                                                                                    std::memory_order_relaxed);
        scopeAt += static_cast<std::uint32_t>(n);
        done += n;
    }
    if (! tone)
        tonePhase_ = 0;
    scopeWrite_.store(scopeAt, std::memory_order_release);

    int on = 0;
    for (const auto& v : voices_)
        on += v.note >= 0 ? 1 : 0;
    voicesOn_.store(on, std::memory_order_relaxed);
    step_.store(seqStep_, std::memory_order_relaxed);
    periods_.fetch_add(1, std::memory_order_relaxed);
    if (io.frames > 0)
    {
        const float load = static_cast<float>((monotonicSeconds() - t0) * sampleRate_ / io.frames);
        loadAvg_.store(loadAvg_.load(std::memory_order_relaxed) * 0.99f + load * 0.01f, std::memory_order_relaxed);
        raise(loadMax_, load);
    }
}

void Synth::scope(float* out, int n) const noexcept
{
    // Triggered like a hardware scope: the window starts at the latest rising zero crossing that still leaves n samples
    // after it (else the last n samples), so a steady tone stands still.
    n = std::min(n, kScopeSize / 2);
    const std::uint32_t end = scopeWrite_.load(std::memory_order_acquire);
    const std::uint32_t first = end - static_cast<std::uint32_t>(2 * n);
    const auto at = [&](std::uint32_t i) { return scope_[i & (kScopeSize - 1)].load(std::memory_order_relaxed); };
    std::uint32_t start = end - static_cast<std::uint32_t>(n);
    for (std::uint32_t i = start; i != first; --i)
        if (at(i - 1) < 0.0f && at(i) >= 0.0f)
        {
            start = i;
            break;
        }
    for (int i = 0; i < n; ++i)
        out[i] = at(start + static_cast<std::uint32_t>(i));
}
} // namespace app
