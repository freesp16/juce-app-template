// The SP-16 display: an 800 × 480 LCD behind an X server without a window manager, and the Vivante GC2000 GPU driver
// in two window-system flavours ("x11", "fb") bound by common/GlDispatch.h. Hands the app's renderer the native
// handles: the shared X11 window, the Vivante framebuffer window and the GL loader.
#pragma once

#include "Platform.h"

namespace platform::imx6
{
std::unique_ptr<DisplayDevice> makeDisplay(std::string& error);
}
