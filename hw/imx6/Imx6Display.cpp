#include "imx6/Imx6Display.h"

#include "common/X11Window.h"
#include "common/GlDispatch.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

// Vivante's fb window-system entry points (libEGL-fb), bound through GlDispatch like every EGL/GLES symbol.
extern "C" {
void* fbGetDisplayByIndex(int);
void fbGetDisplayGeometry(void*, int*, int*);
void* fbCreateWindow(void*, int, int, int, int);
}

namespace platform::imx6
{
using common::openX11Window;
using common::X11Window;
using common::x11Display;
using common::x11Drawable;
using common::x11ScreenSize;
using common::x11Size;

namespace
{
// Two framebuffers (vsynced flips) only when video memory already has room.
bool fbHasTwoBuffers(int index, std::string& line)
{
    char path[32];
    std::snprintf(path, sizeof path, "/dev/fb%d", index);
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0)
    {
        line = std::string("cannot open ") + path;
        return false;
    }
    fb_var_screeninfo v {};
    fb_fix_screeninfo f {};
    const bool ok = ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0;
    ::close(fd);
    if (! ok)
        return false;
    char buf[160];
    std::snprintf(buf, sizeof buf, "fb%d %ux%u %ubpp virtual %ux%u", index, v.xres, v.yres, v.bits_per_pixel, v.xres_virtual,
                  v.yres_virtual);
    line = buf;
    return v.yres_virtual >= 2 * v.yres && f.smem_len >= 2u * f.line_length * v.yres;
}

int fbIndex()
{
    const char* idx = std::getenv("SP16_GL_FB"); // framebuffer index override (device bring-up)
    return idx != nullptr && *idx != 0 ? std::atoi(idx) : 0;
}

class Imx6Display final : public DisplayDevice
{
public:
    Imx6Display()
    {
        sizeKnown_ = x11ScreenSize(w_, h_);
        if (! sizeKnown_)
            w_ = 800, h_ = 480;
    }

    int width() const noexcept override { return w_; }
    int height() const noexcept override { return h_; }
    bool sizeKnown() const noexcept override { return sizeKnown_; }

    platform::RendererEnv rendererEnv() override
    {
        platform::RendererEnv env;
        env.width = w_;
        env.height = h_;
        auto fbLine = std::make_shared<std::string>();
        env.loadGl = [fbLine](std::string_view flavour, std::string& error) {
            if (flavour == "fb")
                // A user value wins (setenv overwrite = 0).
                setenv("FB_MULTI_BUFFER", fbHasTwoBuffers(fbIndex(), *fbLine) ? "2" : "1", 0);
            return gldispatch::load(std::string(flavour).c_str(), error);
        };
        env.glReport = [] { return gldispatch::report(); };
        // The X window is opened lazily, once per process, and shared by every renderer that asks for "x11".
        auto window = window_;
        env.nativeWindow = [window, fbLine](std::string_view kind, std::string& error) -> std::optional<platform::NativeWindow> {
            platform::NativeWindow nw;
            if (kind == "fb")
            {
                void* display = fbGetDisplayByIndex(fbIndex());
                if (display == nullptr)
                    return error = "fbGetDisplayByIndex failed", std::nullopt;
                int w = 0, h = 0;
                fbGetDisplayGeometry(display, &w, &h);
                void* win = fbCreateWindow(display, 0, 0, w, h);
                if (win == nullptr)
                    return error = "fbCreateWindow failed", std::nullopt;
                nw.display = display;
                nw.window = reinterpret_cast<std::uintptr_t>(win);
                nw.width = w;
                nw.height = h;
                nw.info = *fbLine;
                return nw;
            }
            if (kind != "x11" && kind != "x11-input")
                return error = "imx6: no native window of kind " + std::string(kind), std::nullopt;
            if (! window->x)
                window->x = openX11Window(kind == "x11-input", error);
            if (! window->x)
                return std::nullopt;
            nw.display = x11Display(*window->x);
            nw.window = x11Drawable(*window->x);
            x11Size(*window->x, nw.width, nw.height);
            nw.owner = window->x;
            return nw;
        };
        return env;
    }

private:
    struct SharedWindow
    {
        std::shared_ptr<X11Window> x;
    };
    std::shared_ptr<SharedWindow> window_ = std::make_shared<SharedWindow>();
    int w_ = 800, h_ = 480;
    bool sizeKnown_ = false;
};
} // namespace

std::unique_ptr<DisplayDevice> makeDisplay(std::string&) { return std::make_unique<Imx6Display>(); }
} // namespace platform::imx6
