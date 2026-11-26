// The screen drawn by the CPU, for a unit or a machine whose GPU path fails: the same picture as GlDisplay, put in the
// platform's X11 window with XPutImage. On the unit that is the override-redirect window over the screen, on a desktop
// the app's window.
//
// Each frame:
//   1. JUCE's software renderer repaints the parts of the UI that called repaint() into an image, as GlDisplay does,
//   2. those parts, and the scope's box while it traces, are composed again: the background, the scope trace, the UI
//      image over it,
//   3. only the composed rectangles go to the X server, in its own pixel format (16-bit on the unit, 24-bit on most
//      desktops). An idle screen costs nothing. The whole picture is sent again every 2 s, for an X server that lost
//      what the window showed.
// Nothing waits for vsync here: the app's frame loop keeps to 60 Hz itself.
#pragma once

#include "app/ScreenOutput.h"

#include "Platform.h"

#include <memory>
#include <string>

namespace app
{
class CpuDisplay final : public ScreenOutput
{
public:
    CpuDisplay();
    ~CpuDisplay() override;

    // Shows `ui` (800 × 480) in the platform's X11 window from now on. False + reason on failure.
    bool open(const platform::RendererEnv&, juce::Component& ui, std::string& error);
    bool draw(const SceneInput&, const float* scope, int scopeSize) override;
    std::string describe() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace app
