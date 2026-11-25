// What puts the screen on the display, whatever draws it: GlDisplay (OpenGL ES 2) or CpuDisplay (the CPU, into an X11
// window). Both repaint only the parts of the UI that called repaint(), through DirtyAreas.
#pragma once

#include "app/Screen.h"

#include <string>

namespace app
{
class ScreenOutput
{
public:
    virtual ~ScreenOutput() = default;
    // Draws and presents one frame. `scope` = the samples to trace. False when the surface is lost.
    virtual bool draw(const SceneInput&, const float* scope, int scopeSize) = 0;
    virtual std::string describe() const = 0;
};

// The root component's repaint() areas, collected without a desktop window (the hook juce::OpenGLContext uses too).
struct DirtyAreas final : juce::CachedComponentImage
{
    explicit DirtyAreas(juce::Component& c) : root(c) {}
    void paint(juce::Graphics&) override {}
    bool invalidateAll() override { return invalidate(root.getLocalBounds()); }
    bool invalidate(const juce::Rectangle<int>& area) override
    {
        areas.add(area.getIntersection(root.getLocalBounds()));
        return false;
    }
    void releaseResources() override {}
    juce::Component& root;
    juce::RectangleList<int> areas;
};

// Shows `ui` without a desktop window and returns its DirtyAreas (owned by `ui`). It takes no JUCE mouse clicks (touch
// comes from the platform), which also keeps JUCE from creating juce::Desktop, whose X11 windowing crashes on the unit.
inline DirtyAreas* attachUi(juce::Component& ui)
{
    ui.setInterceptsMouseClicks(false, false);
    ui.setVisible(true); // repaint() stops at an invisible component
    auto* dirty = new DirtyAreas(ui);
    ui.setCachedComponentImage(dirty);
    dirty->invalidateAll();
    return dirty;
}

// Repaints the dirty areas into `image` (premultiplied ARGB, the screen's size), clears them and returns them.
inline juce::RectangleList<int> repaintUi(juce::Component& root, DirtyAreas& dirty, juce::Image& image)
{
    juce::RectangleList<int> areas;
    areas.swapWith(dirty.areas);
    if (areas.isEmpty())
        return areas;
    areas.consolidate();
    if (areas.getNumRectangles() > 8) // many small areas cost more in clipping than one box
        areas = juce::RectangleList<int>(areas.getBounds());
    for (const auto& r : areas)
        image.clear(r);
    juce::Graphics g(image);
    g.reduceClipRegion(areas);
    root.paintEntireComponent(g, true);
    return areas;
}
} // namespace app
