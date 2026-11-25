#include "app/CpuDisplay.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace app
{
namespace
{
constexpr int kW = Screen::kWidth, kH = Screen::kHeight;
constexpr double kFullPresentMs = 2000.0;

std::uint32_t rgb(juce::Colour c) { return (std::uint32_t(c.getRed()) << 16) | (std::uint32_t(c.getGreen()) << 8) | c.getBlue(); }

// dst = src + dst × (1 − a), per channel. src: premultiplied, 0..255 per channel. a: 0..255.
inline std::uint32_t over(std::uint32_t dst, std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
{
    const std::uint32_t k = 255 - a;
    const std::uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    return (std::min(255u, r + (dr * k + 127) / 255) << 16) | (std::min(255u, g + (dg * k + 127) / 255) << 8) |
           std::min(255u, b + (db * k + 127) / 255);
}
} // namespace

struct CpuDisplay::Impl
{
    std::shared_ptr<void> window; // keeps the native window open
    ::Display* dpy = nullptr;
    ::Window win = 0;
    GC gc = nullptr;
    XImage* ximage = nullptr;
    int depth = 0, winW = 0, winH = 0;
    int k = 1; // whole pixels of the window a pixel of the picture (a desktop window of 800·k × 480·k)
    std::string info;
    juce::Component* root = nullptr;
    DirtyAreas* dirty = nullptr; // owned by the root component
    juce::Image image {juce::Image::ARGB, kW, kH, true, juce::SoftwareImageType()};
    std::vector<std::uint32_t> frame = std::vector<std::uint32_t>(std::size_t(kW) * kH); // 0x00RRGGBB
    juce::Rectangle<int> lastScope;
    double lastFull = -1e9;

    // The scope as GlDisplay draws it: a soft wide pass, then the core line, the trace magnified up to 4× when quiet.
    // Here one span a pixel column, from the lowest to the highest point the trace reaches in it, widened by the
    // line's half width, with anti-aliased ends. Only the part inside `clip` is drawn.
    void drawScope(const SceneInput& s, const float* scope, int n, const juce::Rectangle<int>& clip)
    {
        const auto box = s.scope.reduced(0, 4);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max(peak, std::fabs(scope[i]));
        const float mid = box.getCentreY(), amp = box.getHeight() * 0.5f / std::max(0.25f, std::min(1.0f, peak));
        const float scale = static_cast<float>(n - 1) / box.getWidth();
        const auto yAt = [&](float f) {
            f = std::clamp(f, 0.0f, static_cast<float>(n - 1));
            const int i = std::min(static_cast<int>(f), n - 2);
            const float t = f - static_cast<float>(i);
            return mid - amp * (scope[i] + (scope[i + 1] - scope[i]) * t);
        };
        const int x0 = std::max(clip.getX(), static_cast<int>(std::floor(box.getX())));
        const int x1 = std::min(clip.getRight(), static_cast<int>(std::ceil(box.getRight())));
        for (const auto& [halfWidth, alpha] : {std::pair {3.0f, 0.16f}, std::pair {0.9f, 1.0f}})
        {
            const std::uint32_t r = s.trace.getRed(), g = s.trace.getGreen(), b = s.trace.getBlue();
            for (int x = x0; x < x1; ++x)
            {
                const float f0 = (static_cast<float>(x) - box.getX()) * scale, f1 = f0 + scale;
                float lo = std::min(yAt(f0), yAt(f1)), hi = std::max(yAt(f0), yAt(f1));
                for (int i = std::max(0, static_cast<int>(std::ceil(f0))); i <= std::min(n - 1, static_cast<int>(f1)); ++i)
                {
                    const float y = mid - amp * scope[i];
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
                lo -= halfWidth;
                hi += halfWidth;
                const int y0 = std::max(clip.getY(), static_cast<int>(std::floor(lo)));
                const int y1 = std::min(clip.getBottom(), static_cast<int>(std::ceil(hi)));
                for (int y = y0; y < y1; ++y)
                {
                    const float cover = std::min(hi, y + 1.0f) - std::max(lo, static_cast<float>(y)); // 0..1
                    const auto a = static_cast<std::uint32_t>(std::clamp(cover, 0.0f, 1.0f) * alpha * 255.0f + 0.5f);
                    auto& px = frame[std::size_t(y) * kW + x];
                    px = over(px, r * a / 255, g * a / 255, b * a / 255, a);
                }
            }
        }
    }

    // The UI image over the frame, inside `r` (JUCE's ARGB words are premultiplied: 0xAARRGGBB).
    void blendUi(const juce::Image::BitmapData& bits, const juce::Rectangle<int>& r)
    {
        for (int y = r.getY(); y < r.getBottom(); ++y)
        {
            const auto* src = reinterpret_cast<const std::uint32_t*>(bits.getLinePointer(y));
            auto* dst = &frame[std::size_t(y) * kW];
            for (int x = r.getX(); x < r.getRight(); ++x)
            {
                const std::uint32_t p = src[x], a = p >> 24;
                if (a == 255)
                    dst[x] = p & 0xFFFFFF;
                else if (a != 0 || (p & 0xFFFFFF) != 0)
                    dst[x] = over(dst[x], (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, a);
            }
        }
    }

    // `r` of the frame into the X image, in the server's format, each pixel k × k, and to the window.
    void present(const juce::Rectangle<int>& r)
    {
        const std::size_t bytes = std::size_t(ximage->bits_per_pixel / 8);
        for (int y = r.getY(); y < r.getBottom(); ++y)
        {
            const std::uint32_t* src = &frame[std::size_t(y) * kW];
            char* line = ximage->data + std::size_t(y) * k * ximage->bytes_per_line;
            if (ximage->bits_per_pixel == 16)
            {
                auto* out = reinterpret_cast<std::uint16_t*>(line);
                for (int x = r.getX(); x < r.getRight(); ++x)
                {
                    const std::uint32_t p = src[x];
                    std::fill_n(out + x * k, k,
                                static_cast<std::uint16_t>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F)));
                }
            }
            else if (k == 1)
                std::copy(src + r.getX(), src + r.getRight(), reinterpret_cast<std::uint32_t*>(line) + r.getX());
            else
                for (int x = r.getX(); x < r.getRight(); ++x)
                    std::fill_n(reinterpret_cast<std::uint32_t*>(line) + x * k, k, src[x]);
            for (int j = 1; j < k; ++j) // the same line k times
                std::copy_n(line + std::size_t(r.getX()) * k * bytes, std::size_t(r.getWidth()) * k * bytes,
                            line + std::size_t(j) * ximage->bytes_per_line + std::size_t(r.getX()) * k * bytes);
        }
        XPutImage(dpy, win, gc, ximage, r.getX() * k, r.getY() * k, r.getX() * k, r.getY() * k,
                  static_cast<unsigned>(r.getWidth() * k), static_cast<unsigned>(r.getHeight() * k));
    }
};

CpuDisplay::CpuDisplay() : impl_(std::make_unique<Impl>()) {}

CpuDisplay::~CpuDisplay()
{
    auto& d = *impl_;
    if (d.root != nullptr)
        d.root->setCachedComponentImage(nullptr);
    if (d.ximage != nullptr)
        XDestroyImage(d.ximage); // frees its pixels too (malloc'd below)
    if (d.gc != nullptr)
        XFreeGC(d.dpy, d.gc);
}

bool CpuDisplay::open(const platform::RendererEnv& env, juce::Component& ui, std::string& error)
{
    auto& d = *impl_;
    if (! env.nativeWindow)
        return error = "no native window on this platform", false;
    const auto window = env.nativeWindow("x11", error);
    if (! window)
        return false;
    d.window = window->owner;
    d.dpy = static_cast<::Display*>(window->display);
    d.win = static_cast<::Window>(window->window);
    d.winW = window->width;
    d.winH = window->height;
    if (d.winW < kW || d.winH < kH)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "the X window is %dx%d, smaller than %dx%d", d.winW, d.winH, kW, kH);
        return error = buf, false;
    }

    d.k = std::max(1, std::min(d.winW / kW, d.winH / kH)); // a desktop window at scale=2…4

    const int screen = DefaultScreen(d.dpy);
    d.depth = DefaultDepth(d.dpy, screen);
    if (d.depth != 16 && d.depth != 24 && d.depth != 32)
        return error = "X depth " + std::to_string(d.depth) + " is not supported", false;
    d.ximage = XCreateImage(d.dpy, DefaultVisual(d.dpy, screen), static_cast<unsigned>(d.depth), ZPixmap, 0, nullptr,
                            static_cast<unsigned>(kW * d.k), static_cast<unsigned>(kH * d.k), 32, 0);
    if (d.ximage == nullptr)
        return error = "XCreateImage failed", false;
    if (d.ximage->bits_per_pixel != (d.depth == 16 ? 16 : 32) || d.ximage->byte_order != LSBFirst)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "X image format %d bits a pixel, byte order %d, not supported", d.ximage->bits_per_pixel,
                      d.ximage->byte_order);
        return error = buf, false;
    }
    d.ximage->data = static_cast<char*>(std::calloc(std::size_t(d.ximage->bytes_per_line) * kH * d.k, 1));
    if (d.ximage->data == nullptr)
        return error = "no memory for the frame", false;
    d.gc = XCreateGC(d.dpy, d.win, 0, nullptr);

    d.root = &ui;
    d.dirty = attachUi(ui);

    char text[160];
    std::snprintf(text, sizeof text, "cpu XPutImage, X depth %d (%s), window %dx%d, pixels x%d, %s", d.depth,
                  d.depth == 16 ? "RGB565" : "XRGB8888", d.winW, d.winH, d.k, window->info.c_str());
    d.info = text;
    return true;
}

bool CpuDisplay::draw(const SceneInput& s, const float* scope, int scopeSize)
{
    auto& d = *impl_;
    juce::RectangleList<int> damage = repaintUi(*d.root, *d.dirty, d.image);
    // The scope's box changes every frame while it traces, and once more after it stops, to clear the last trace.
    const auto scopeBox = s.scope.getSmallestIntegerContainer().getIntersection({0, 0, kW, kH});
    if (scopeSize > 1)
        damage.add(scopeBox);
    if (! d.lastScope.isEmpty())
        damage.add(d.lastScope);
    d.lastScope = scopeSize > 1 ? scopeBox : juce::Rectangle<int>();
    const bool full = s.nowMs - d.lastFull >= kFullPresentMs;
    if (damage.isEmpty() && ! full)
        return true;
    damage.consolidate();

    const juce::Image::BitmapData bits(d.image, juce::Image::BitmapData::readOnly);
    const std::uint32_t background = rgb(s.background);
    for (const auto& r : damage)
    {
        for (int y = r.getY(); y < r.getBottom(); ++y)
            std::fill_n(&d.frame[std::size_t(y) * kW + r.getX()], r.getWidth(), background);
        if (scopeSize > 1)
            d.drawScope(s, scope, scopeSize, r);
        d.blendUi(bits, r);
    }
    if (full)
    {
        d.present({0, 0, kW, kH});
        d.lastFull = s.nowMs;
    }
    else
        for (const auto& r : damage)
            d.present(r);
    XFlush(d.dpy);
    return true;
}

std::string CpuDisplay::describe() const { return impl_->info; }
} // namespace app
