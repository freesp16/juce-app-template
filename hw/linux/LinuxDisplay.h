// The Linux desktop display: a normal managed X11 window of 800 × 480 × `scale` pixels (title =
// identity::displayName, fixed size, WM_DELETE_WINDOW = quit, visible cursor), opened lazily, once per process. The
// window also carries the keyboard and the mouse (linux/X11Input.h), which reads the same shared holder.
//
// The app draws into it with the same GLES2 renderer as on the unit, through common/GlDispatch.h with the Mesa
// flavour (libEGL.so.1 + libGLESv2.so.2, EGL_PLATFORM=x11).
#pragma once

#include "Platform.h"
#include "common/X11Window.h"

#include <memory>
#include <string>

namespace platform::linux_desktop
{
struct LinuxWindow
{
    std::shared_ptr<common::X11Window> x; // null until a renderer asked for it
    int scale = 1;
    std::string title{};
    std::shared_ptr<common::X11Window> open(std::string& error);
};

std::unique_ptr<DisplayDevice> makeLinuxDisplay(std::shared_ptr<LinuxWindow> window);
} // namespace platform::linux_desktop
