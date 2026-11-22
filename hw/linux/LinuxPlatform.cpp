// A Linux desktop standing in for the unit: an 800 × 480 (× `scale`) X11 window drawn with Mesa's OpenGL ES, the
// computer keyboard as the panel (linux/KeyMap.cpp), the mouse as the LCD touch, Ctrl + wheel as the touch strip,
// and ALSA `default` (PipeWire / PulseAudio) as the codec's MAIN output. No lamps.
//
// Settings (key=value in ~/.config/<name>/<name>.conf, or <PREFIX>_<KEY> in the environment):
//   audio_device=<ALSA PCM>       default `default`
//   audio_channels=N, audio_map=  a multi-channel interface, e.g. audio_channels=8 audio_map=out34:2/3,out56:4/5
//   scale=1..4                    window = 800·scale × 480·scale pixels
#include "Platform.h"

#include "common/AlsaOutput.h"
#include "linux/HostPaths.h"
#include "linux/LinuxDisplay.h"
#include "linux/X11Input.h"

#include "ProjectIdentity.h"

#include <algorithm>
#include <cstdlib>

namespace platform::linux_desktop
{
namespace
{
std::string setting(const Settings& s, const std::string& key, const std::string& fallback)
{
    auto it = s.find(key);
    return it != s.end() && ! it->second.empty() ? it->second : fallback;
}

class LinuxPlatform final : public Platform
{
public:
    LinuxPlatform() { window_->title = identity::displayName; }

    std::string_view id() const noexcept override { return "linux"; }
    bool detect() const override { return std::getenv("DISPLAY") != nullptr; }
    std::string describe() const override
    {
        return "linux desktop, audio alsa " + device_ + " (" + std::to_string(channels_) + " ch" + (mapNote_.empty() ? "" : ", " + mapNote_)
             + "), X11 window " + std::to_string(800 * window_->scale) + "x" + std::to_string(480 * window_->scale)
             + ", keyboard + mouse";
    }
    Paths paths() const override { return host::xdgPaths(); }

    void configure(const Settings& settings) override
    {
        device_ = setting(settings, "audio_device", "default");
        window_->scale = std::clamp(std::atoi(setting(settings, "scale", "1").c_str()), 1, 4);
        channels_ = static_cast<unsigned>(std::clamp(std::atoi(setting(settings, "audio_channels", "2").c_str()), 2, 32));
        map_ = common::mainOnly(0, 1);
        mapNote_.clear();
        if (const std::string map = setting(settings, "audio_map", ""); ! map.empty())
            mapNote_ = common::parsePortMap(map, map_) ? "audio_map " + map : "audio_map " + map + " IGNORED (bad syntax)";
    }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        if (channels_ != 2 || ! mapNote_.empty()) // a multi-channel interface first, then plain stereo
            devices.push_back(std::make_unique<common::AlsaOutput>(common::AlsaOutputConfig {device_, channels_, 0, 0, map_}));
        devices.push_back(std::make_unique<common::AlsaOutput>(common::AlsaOutputConfig {device_, 2, 0, 0, common::mainOnly(0, 1)}));
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        sources.push_back(std::make_unique<X11InputSource>(env.triggers, window_));
        return sources;
    }

    std::unique_ptr<io::LampSink> lamps() override { return nullptr; }
    std::unique_ptr<DisplayDevice> display(std::string&) override { return makeLinuxDisplay(window_); }

private:
    std::shared_ptr<LinuxWindow> window_ = std::make_shared<LinuxWindow>();
    std::string device_ = "default", mapNote_;
    unsigned channels_ = 2;
    common::AlsaPortMap map_ = common::mainOnly(0, 1);
};
} // namespace
} // namespace platform::linux_desktop

std::unique_ptr<platform::Platform> platform::createPlatform()
{
    return std::make_unique<linux_desktop::LinuxPlatform>();
}
