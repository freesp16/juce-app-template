// The hardware layer's whole contract with the app: one Platform per machine, chosen at build time
// (hw/imx6 = the unit, an SP-16 or a DJS-1000 found at run time; hw/linux = a Linux desktop standing in for one).
//
//   createPlatform()        the machine
//   configure(settings)     key=value settings, before the rest
//   audioDevices()          the codec; start(config, processor) runs AudioProcessor::process() once per period on the
//                           only real-time thread
//   controlSources(env)     the panel, the touch strip, the LCD touch (desktop: keyboard and mouse); pad hits and strip
//                           moves also go straight to env.triggers from their reader threads (the lowest latency)
//   lamps()                 the panel's LEDs (null on a desktop)
//   display()               what a GL renderer needs: the driver flavour loader and the native window
// MIDI is not part of Platform: ALSA raw MIDI works the same on both (hw/common/MidiPort.h; DIN and USB on the unit).
#pragma once

#include "controls/ControlNames.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform
{
// ---- audio ----

// Logical stereo outputs. On the SP-16: MAIN, OUT 3-4, 5-6, 7-8 on the AK4458, PHONES on the AK4452, the analog filter's
// send (only with filter_loop=1). On the DJS-1000: MAIN and PHONES. On a desktop: MAIN only.
enum class AudioPort : std::uint8_t { Main, Out34, Out56, Out78, Phones, FilterSend, Count };
constexpr int kAudioPorts = static_cast<int>(AudioPort::Count);

// out[2 * port] = left, out[2 * port + 1] = right of each AudioPort (planar float); a null pair = the port is absent.
// frames: up to prepare()'s maximum. hostTimeNs: monotonic time at the period start.
struct AudioIO { float* const* out; int numOutPorts; const float* const* in; int numIn; int frames; std::uint64_t hostTimeNs; };

class AudioProcessor
{
public:
    virtual ~AudioProcessor() = default;
    virtual void prepare(double sampleRate, int maxDeviceFrames) = 0; // message thread, may allocate
    virtual void process(const AudioIO&) noexcept = 0;                // audio thread: no allocation, locks or I/O
};

// A live trigger from a reader thread. Pad hit: track = pad = 0–15, velocity 1–127. Strip: pad = kStripTriggerPad,
// velocity = position 0–127, note = contact 0/1. Pad pressure: pad = kPressureTriggerPad, track = pad, velocity = 0–127.
struct TriggerNow { std::uint8_t track, velocity, note; std::int8_t pad; };
constexpr std::int8_t kStripTriggerPad = -16, kPressureTriggerPad = -17;
constexpr TriggerNow stripTrigger(int position, bool contact) noexcept
{
    const int p = position < 0 ? 0 : (position > 127 ? 127 : position);
    return {0, static_cast<std::uint8_t>(p), static_cast<std::uint8_t>(contact ? 1 : 0), kStripTriggerPad};
}
constexpr TriggerNow pressureTrigger(int pad0, int pressure) noexcept
{
    return {static_cast<std::uint8_t>(pad0 & 15), static_cast<std::uint8_t>(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure)),
            0, kPressureTriggerPad};
}

// Receives TriggerNow from several reader threads at once; never blocks; false when full.
class TriggerSink
{
public:
    virtual ~TriggerSink() = default;
    virtual bool push(const TriggerNow&) noexcept = 0;
};

struct PortInfo { AudioPort port; std::int8_t left, right; }; // physical interleaved channels, -1 = absent
struct AudioConfig { unsigned sampleRate = 44100, periodFrames = 256, periods = 3; int rtPriority = 70; };
struct AudioStats { std::uint64_t periods = 0, xruns = 0; unsigned sampleRate = 0, periodFrames = 0; bool realtime = false; };

class AudioDevice
{
public:
    virtual ~AudioDevice() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual std::span<const PortInfo> ports() const noexcept = 0;
    // Calls processor.prepare() on this thread, then starts the device's real-time thread.
    virtual bool start(const AudioConfig&, AudioProcessor&, std::string& info) = 0;
    virtual void stop() = 0;
    virtual AudioStats stats() const noexcept = 0;
};

// ---- display ----

// A native window (or framebuffer surface) for EGL: the EGLNativeDisplayType (X11 Display*, Vivante fb display) and
// EGLNativeWindowType (X11 Window id, fb window pointer). `owner` keeps it alive.
struct NativeWindow
{
    void* display = nullptr;
    std::uintptr_t window = 0;
    int width = 0, height = 0;
    std::string info {};
    std::shared_ptr<void> owner {};
};

struct RendererEnv
{
    int width = 800, height = 480;
    // Binds the GL/EGL driver flavour ("fb" or "x11") for this process: one flavour per process (common/GlDispatch.h).
    std::function<bool(std::string_view flavour, std::string& error)> loadGl;
    std::function<std::string()> glReport;
    // A native surface: "x11" (a window) or "fb" (the framebuffer; after loadGl("fb")).
    std::function<std::optional<NativeWindow>(std::string_view kind, std::string& error)> nativeWindow;
};

class DisplayDevice
{
public:
    virtual ~DisplayDevice() = default;
    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual RendererEnv rendererEnv() = 0;
    virtual bool sizeKnown() const noexcept { return true; }
};

// ---- the unit ----

// The models this hardware layer knows. Both are the same i.MX6 board with the same screen, GPU and panel link; their
// panels report different controls, their codecs have different outputs (hw/imx6/PanelRoles.h, Imx6Platform.cpp).
enum class Unit : std::uint8_t { Sp16, Djs1000 };
constexpr std::string_view unitKey(Unit u) noexcept { return u == Unit::Djs1000 ? "djs1000" : "sp16"; }
constexpr std::string_view unitModel(Unit u) noexcept { return u == Unit::Djs1000 ? "DJS-1000" : "TORAIZ SP-16"; }
constexpr std::optional<Unit> unitFromKey(std::string_view k) noexcept
{
    if (k == "djs1000" || k == "djs" || k == "DJS-1000")
        return Unit::Djs1000;
    if (k == "sp16" || k == "SP-16")
        return Unit::Sp16;
    return std::nullopt;
}

// What the unit has, so an app can leave out what it cannot map (a control that is not there, a lamp that cannot
// light, an output jack that is absent: AudioDevice::ports()).
struct Capabilities
{
    Unit unit = Unit::Sp16;
    std::string model {};                                   // "TORAIZ SP-16", "DJS-1000"
    std::array<bool, io::kLampSlots> controls {};           // produces events (by io::lampSlot)
    std::array<bool, io::kLampSlots> lamps {};              // can be lit
    bool has(io::Control c) const noexcept { return slot(c) >= 0 && controls[static_cast<std::size_t>(slot(c))]; }
    bool lights(io::Control c) const noexcept { return slot(c) >= 0 && lamps[static_cast<std::size_t>(slot(c))]; }

private:
    static int slot(io::Control c) noexcept
    {
        const int s = io::lampSlot(c);
        return s >= 0 && s < io::kLampSlots ? s : -1;
    }
};
// The panel of `unit` as capabilities (hw/imx6/PanelRoles.cpp; the desktop uses it too, to stand in for that unit).
Capabilities panelCapabilities(Unit unit);

// ---- the machine ----

struct Paths { std::string dataDir, settingsFile, cacheDir; std::vector<std::string> usbMounts; };
struct ControlEnv { TriggerSink* triggers; };
using Settings = std::map<std::string, std::string, std::less<>>;

class Platform
{
public:
    virtual ~Platform() = default;
    virtual std::string_view id() const noexcept = 0;   // "imx6", "linux"
    virtual std::string describe() const = 0;           // one log line
    virtual bool detect() const = 0;                    // true on matching hardware
    virtual Paths paths() const = 0;
    virtual void configure(const Settings&) {}
    virtual std::vector<std::unique_ptr<AudioDevice>> audioDevices() = 0; // in fallback order
    virtual std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv&) = 0;
    virtual std::unique_ptr<io::LampSink> lamps() = 0;  // null = no lamps
    virtual std::unique_ptr<DisplayDevice> display(std::string& error) = 0;
    // The unit and what it has (after configure()). The desktop stands in for the SP-16 unless unit=djs1000.
    virtual Capabilities capabilities() const = 0;
};

std::unique_ptr<Platform> createPlatform(); // imx6/Imx6Platform.cpp or linux/LinuxPlatform.cpp, per build

// The desktop window's close button asks the app to quit (any thread).
inline std::atomic<bool> quitFlag {false};
inline void requestQuit() noexcept { quitFlag.store(true); }
inline bool quitRequested() noexcept { return quitFlag.load(); }
} // namespace platform
