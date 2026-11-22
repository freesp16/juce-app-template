#include "linux/LinuxDisplay.h"

#include "common/GlDispatch.h"

#include <cstdlib>

namespace platform::linux_desktop
{
std::shared_ptr<common::X11Window> LinuxWindow::open(std::string& error)
{
    if (! x)
    {
        common::X11WindowOptions o;
        o.managed = true;
        o.width = 800 * scale;
        o.height = 480 * scale;
        o.title = title;
        x = common::openX11Window(o, error);
    }
    return x;
}

namespace
{
class LinuxDisplay final : public DisplayDevice
{
public:
    explicit LinuxDisplay(std::shared_ptr<LinuxWindow> window) : window_(std::move(window)) {}

    int width() const noexcept override { return 800 * window_->scale; }
    int height() const noexcept override { return 480 * window_->scale; }
    bool sizeKnown() const noexcept override { return true; } // the window's size is ours, not the screen's

    platform::RendererEnv rendererEnv() override
    {
        platform::RendererEnv env;
        env.width = width();
        env.height = height();
        env.loadGl = [](std::string_view flavour, std::string& error) {
            if (flavour != "x11")
            {
                error = "linux: no GL flavour '" + std::string(flavour) + "' (only x11: EGL on the window)";
                return false;
            }
            setenv("EGL_PLATFORM", "x11", 0); // Mesa: the native display is an X11 Display* (a user value wins)
            return gldispatch::loadLibraries("mesa", gldispatch::kMesa, error);
        };
        env.glReport = [] { return gldispatch::report(); };
        auto window = window_;
        env.nativeWindow = [window](std::string_view kind, std::string& error) -> std::optional<platform::NativeWindow> {
            if (kind != "x11")
                return error = "linux: no native window of kind " + std::string(kind), std::nullopt;
            auto x = window->open(error);
            if (! x)
                return std::nullopt;
            platform::NativeWindow nw;
            nw.display = common::x11Display(*x);
            nw.window = common::x11Drawable(*x);
            common::x11Size(*x, nw.width, nw.height);
            nw.info = "x11 window " + std::to_string(nw.width) + "x" + std::to_string(nw.height);
            nw.owner = x;
            return nw;
        };
        return env;
    }

private:
    std::shared_ptr<LinuxWindow> window_;
};
} // namespace

std::unique_ptr<DisplayDevice> makeLinuxDisplay(std::shared_ptr<LinuxWindow> window)
{
    return std::make_unique<LinuxDisplay>(std::move(window));
}
} // namespace platform::linux_desktop
