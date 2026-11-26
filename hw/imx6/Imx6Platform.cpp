// The unit as a Platform: NXP i.MX6Q (Cortex-A9 ×4, NEON), the subucom SPI panel, the capacitive touch strip, the
// Vivante GC2000 GPU via common/GlDispatch.h, an X server without a window manager. Named after the SoC, not after the
// product: the same board is the TORAIZ SP-16 and the DJS-1000, found at start (detectUnit) and told apart by:
//
//   SP-16      AK4458 codec, 16 channels: MAIN, OUT 3-4, 5-6, 7-8; PHONES on hw:ak4452audio,0; the analog filter
//              (its send/return on hw:ak4452audio,1, its select line and HP CUTOFF DAC: AnalogBoard)
//   DJS-1000   MAIN (master) on hw:ak4452audio,1 and PHONES on hw:ak4452audio,0, both stereo S24_LE; the panel holds the
//              outputs muted until the program releases them; PHONES LEVEL is a pot the program applies; the power key
//              asks the panel for standby
//
// Each unit's panel is in PanelRoles.h. capabilities() tells the app what this unit has.
#include "Platform.h"

#include "common/AlsaOutput.h"
#include "imx6/Imx6Display.h"
#include "imx6/AnalogBoard.h"
#include "imx6/PanelLamps.h"
#include "imx6/PanelLink.h"
#include "imx6/StripSource.h"

#include "ProjectIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace platform::imx6
{
namespace
{
constexpr const char* panelDevice = "/dev/subucom_spi0.1";

// The SP-16 codec's logical outputs: 16 S16 channels in 8 two-channel DAC slots where only the even channel carries
// audio, MAIN = 12/14 (observed), OUT3-4 = 8/10 and OUT5-6 = 4/6 (inferred, not verified), OUT7-8 = 0/2; the odd
// channels are padding written as 0. PHONES is the separate PCM hw:ak4452audio,0 channels 0/1
// (AlsaOutputConfig::auxDevice); the analog filter's send channel is unknown. The map is data: the setting audio_map
// (e.g. "out34:8/10,out56:4/6") corrects it without a rebuild.
constexpr common::AlsaPortMap kCodecMap {{{AudioPort::Main, 12, 14},
                                         {AudioPort::Out34, 8, 10},
                                         {AudioPort::Out56, 4, 6},
                                         {AudioPort::Out78, 0, 2},
                                         {AudioPort::Phones, -1, -1},
                                         {AudioPort::FilterSend, -1, -1}}};

// The unit: BOOTMENU_UNIT (set by the boot menu), else the official application that is installed, else the SP-16's
// codec, else the SP-16. The setting unit= overrides it (configure()).
Unit detectUnit(std::string& why)
{
    if (const char* env = std::getenv("BOOTMENU_UNIT"); env != nullptr)
        if (const auto u = unitFromKey(env))
        {
            why = std::string("BOOTMENU_UNIT=") + env;
            return *u;
        }
    if (::access("/root/pdj/EP113_Main_R", F_OK) == 0)
        return why = "EP113_Main_R installed", Unit::Djs1000;
    if (::access("/root/pdj/EP085_Main_R", F_OK) == 0)
        return why = "EP085_Main_R installed", Unit::Sp16;
    if (::access("/proc/asound/ak4458audio", F_OK) == 0)
        return why = "AK4458 codec", Unit::Sp16;
    return why = "nothing to tell, assumed", Unit::Sp16;
}

// DJS-1000: its panel keeps the outputs muted while the program's frames ask for it (LampBus flags), so the request is
// released once the audio runs and made again before it stops.
class ReleasesMute final : public AudioDevice
{
public:
    ReleasesMute(std::unique_ptr<AudioDevice> inner, std::shared_ptr<panel::LampBus> bus)
        : inner_(std::move(inner)), bus_(std::move(bus))
    {
    }
    ~ReleasesMute() override { stop(); }
    std::string_view id() const noexcept override { return inner_->id(); }
    std::span<const PortInfo> ports() const noexcept override { return inner_->ports(); }
    bool start(const AudioConfig& c, AudioProcessor& p, std::string& info) override
    {
        const bool ok = inner_->start(c, p, info);
        if (ok)
        {
            bus_->setOutputsLive(true);
            info += ", panel mute released";
        }
        return ok;
    }
    void stop() override
    {
        bus_->setOutputsLive(false);
        inner_->stop();
    }
    AudioStats stats() const noexcept override { return inner_->stats(); }

private:
    std::unique_ptr<AudioDevice> inner_;
    std::shared_ptr<panel::LampBus> bus_;
};

class Imx6Platform final : public Platform
{
public:
    Imx6Platform() { setUnit(detectUnit(unitWhy_)); }

    std::string_view id() const noexcept override { return "imx6"; }

    // `/dev/subucom_spi0.1` or the codec exists. On an emulated unit the device nodes may be served by an LD_PRELOAD
    // shim and not exist on disk, so the board's backlight node (harmless to open) is also probed by opening it.
    bool detect() const override { return !detectReason().empty(); }

    std::string describe() const override
    {
        const auto why = detectReason();
        std::string audio;
        if (unit_ == Unit::Djs1000)
            audio = "audio " + audioDevice_ + " (MAIN, S24_LE)" + (mapNote_.empty() ? "" : " (" + mapNote_ + ")")
                  + ", phones " + (phonesDevice_.empty() ? std::string("off") : phonesDevice_)
                  + (phonesKnob_ ? " at the PHONES LEVEL knob" : "");
        else
            audio = "audio " + audioDevice_ + " (MAIN = ch 12/14 of 16; OUT3-4 8/10, OUT5-6 4/6, OUT7-8 0/2"
                  + (mapNote_.empty() ? "" : "; " + mapNote_) + "), phones "
                  + (phonesDevice_.empty() ? std::string("off") : phonesDevice_) + ", filter loop "
                  + (loopDevice_.empty() ? std::string("off") : loopDevice_);
        return std::string(unitModel(unit_)) + " (" + unitWhy_ + "), i.MX6 board ("
             + (why.empty() ? std::string("not detected") : why) + "), " + audio + ", panel " + panelDevice
             + " (+ lamps, LCD touch), input trace " + (inputTrace_ ? "on" : "off") + ", strip /dev/cdc_spi2.0"
             + ", display X11 + Vivante GC2000 (gldispatch)";
    }

    Paths paths() const override
    {
        // Settings: key=value lines in /opt/sp16/dev-share/<name>.conf when that development share exists, otherwise
        // <data dir>/<name>.conf. The app reads the file on every start; <PREFIX>_<KEY> environment variables
        // override it.
        Paths p;
        p.dataDir = std::string("/root/data/") + identity::name; // on the unit's data volume
        p.cacheDir = p.dataDir + "/cache";
        const std::string overlay = std::string("/opt/sp16/dev-share/") + identity::name + ".conf";
        p.settingsFile = ::access(overlay.c_str(), F_OK) == 0 ? overlay : p.dataDir + "/" + identity::name + ".conf";
        return p;
    }

    void configure(const Settings& settings) override
    {
        // unit=sp16|djs1000: which panel and codec, when the detection is wrong (before anything starts)
        if (auto it = settings.find("unit"); it != settings.end())
            if (const auto u = unitFromKey(it->second); u && *u != unit_)
            {
                unitWhy_ = "setting unit=" + it->second;
                setUnit(*u);
            }
        if (auto it = settings.find("audio_device"); it != settings.end() && ! it->second.empty())
            audioDevice_ = it->second;
        if (auto it = settings.find("input_trace"); it != settings.end()) // INPUT log lines, default on
            inputTrace_ = it->second != "0";
        // phones_device=<PCM> (default hw:ak4452audio,0; "none" = no headphone stream) and
        // audio_map=<port>:<L>/<R>,… over the codec map (kCodecMap; DJS-1000: main:0/1).
        if (auto it = settings.find("phones_device"); it != settings.end())
            phonesDevice_ = it->second == "none" ? std::string() : it->second;
        phonesS16_ = false; // phones_format=s16: a 16-bit stream (default S24_LE, as the stock firmware)
        if (auto it = settings.find("phones_format"); it != settings.end())
            phonesS16_ = it->second == "s16" || it->second == "S16_LE";
        if (auto it = settings.find("phones_gain"); it != settings.end()) // linear, default 0.5 (−6 dB)
            phonesGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        // DJS-1000: phones_knob=0 leaves PHONES at phones_gain whatever the PHONES LEVEL knob says (default: the knob)
        if (auto it = settings.find("phones_knob"); it != settings.end())
            phonesKnob_ = it->second != "0";
        // The SP-16's analog filter send/return loop: OPT-IN (filter_loop=1 = hw:ak4452audio,1 "Send & Return", or a
        // PCM name; default none). The first unit run with it on by default stopped right after the audio started, so
        // it stays off until proven on hardware. Its two controls (panel::AnalogBoard: the filter-select GPIO line while
        // ACTIVE is lit, the HP CUTOFF DAC word) follow the loop unless filter_board=0/1 says otherwise. The DJS-1000
        // has no analog filter (and its hw:ak4452audio,1 is the master).
        loopDevice_.clear();
        if (unit_ == Unit::Sp16)
            if (auto it = settings.find("filter_loop"); it != settings.end() && it->second != "none" && it->second != "0")
                loopDevice_ = it->second == "1" ? std::string("hw:ak4452audio,1") : it->second;
        bool board = ! loopDevice_.empty();
        if (auto it = settings.find("filter_board"); it != settings.end() && unit_ == Unit::Sp16)
            board = it->second != "0";
        board_->setEnabled(board);
        map_ = unit_ == Unit::Djs1000 ? common::mainOnly(0, 1) : kCodecMap;
        mapNote_.clear();
        if (auto it = settings.find("audio_map"); it != settings.end() && ! it->second.empty())
            mapNote_ = common::parsePortMap(it->second, map_) ? "audio_map " + it->second
                                                              : "audio_map " + it->second + " IGNORED (bad syntax)";
        // audio_period / audio_periods: the period size and count (<PREFIX>_AUDIO_PERIOD still wins in AlsaOutput).
        auto number = [&](const char* key) {
            auto it = settings.find(key);
            return it == settings.end() ? 0u : static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 10));
        };
        period_ = number("audio_period");
        periods_ = number("audio_periods");
    }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        if (unit_ == Unit::Djs1000)
        {
            // MAIN on the master PCM, PHONES on the second one, at the PHONES LEVEL knob
            common::AlsaOutputConfig c {audioDevice_, 2, period_, periods_, map_, phonesDevice_, 2, AudioPort::Phones,
                                        phonesGain_, phonesS16_, {}};
            c.mainS24 = true;
            if (phonesKnob_)
                c.auxLevel = phonesLevel_;
            devices.push_back(std::make_unique<ReleasesMute>(std::make_unique<common::AlsaOutput>(c), lampBus_));
            return devices;
        }
        // Direct ALSA hw device: the codec's 16 channels (MAIN = 12/14, the other output pairs) + PHONES
        devices.push_back(std::make_unique<common::AlsaOutput>(
            common::AlsaOutputConfig {audioDevice_, 16, period_, periods_, map_, phonesDevice_, 2, AudioPort::Phones,
                                      phonesGain_, phonesS16_, loopDevice_}));
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        auto panel = std::make_unique<PanelLink>(unit_, panel::panelMap(unit_).pads, env.triggers, lampBus_,
                                                 inputTrace_, unit_ == Unit::Sp16 ? board_ : nullptr); // + LCD touch
        if (unit_ == Unit::Djs1000)
            panel->setPhonesLevel(phonesLevel_);
        sources.push_back(std::move(panel));
        sources.push_back(std::make_unique<StripSource>(lampBus_, inputTrace_, env.triggers)); // /dev/cdc_spi2.0
        return sources;
    }

    // The panel's TX lamp frames; the panel link sends them (on change + 4 Hz heartbeat, local echo until a flush).
    std::unique_ptr<io::LampSink> lamps() override
    {
        return std::make_unique<panel::PanelLamps>(lampBus_, unit_ == Unit::Sp16 ? board_ : nullptr);
    }

    std::unique_ptr<DisplayDevice> display(std::string& error) override { return makeDisplay(error); }

    Capabilities capabilities() const override { return panelCapabilities(unit_); }

private:
    void setUnit(Unit u)
    {
        unit_ = u;
        lampBus_ = std::make_shared<panel::LampBus>(u);
        audioDevice_ = u == Unit::Djs1000 ? "hw:ak4452audio,1" : "hw:ak4458audio,0";
        map_ = u == Unit::Djs1000 ? common::mainOnly(0, 1) : kCodecMap;
    }

    static std::string detectReason()
    {
        for (const char* path : {panelDevice, "/proc/asound/ak4458audio", "/proc/asound/ak4452audio", "/dev/cdc_spi2.0"})
            if (::access(path, F_OK) == 0)
                return std::string(path) + " present";
        const int fd = ::open("/dev/backlight", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
        {
            ::close(fd);
            return "/dev/backlight opens";
        }
        return {};
    }

    Unit unit_ = Unit::Sp16;
    std::string unitWhy_;
    std::string audioDevice_ = "hw:ak4458audio,0";
    std::string phonesDevice_ = "hw:ak4452audio,0"; // the headphone DAC
    float phonesGain_ = 0.5f;                       // a fixed −6 dB on the headphone DAC
    bool phonesS16_ = false;
    bool phonesKnob_ = true;                        // DJS-1000: PHONES follows its PHONES LEVEL knob
    // DJS-1000: the PHONES LEVEL knob as a gain, written by the panel's reader thread, read by the audio thread; 0
    // until the panel's first frame says where the knob is
    std::shared_ptr<std::atomic<float>> phonesLevel_ = std::make_shared<std::atomic<float>>(0.0f);
    std::string loopDevice_;                        // the SP-16's analog filter send/return (opt-in: filter_loop=1)
    std::shared_ptr<panel::AnalogBoard> board_ = std::make_shared<panel::AnalogBoard>();
    common::AlsaPortMap map_ = kCodecMap;
    std::string mapNote_;
    bool inputTrace_ = true;
    unsigned period_ = 0, periods_ = 0; // 0 = AudioConfig's (256 × 3)
    std::shared_ptr<panel::LampBus> lampBus_;  // panel link + lamps + strip echo (setUnit)
};

} // namespace
} // namespace platform::imx6

std::unique_ptr<platform::Platform> platform::createPlatform() { return std::make_unique<imx6::Imx6Platform>(); }
