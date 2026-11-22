// Run-time binding of EGL/GLES to one driver flavour: a Vivante flavour on imx6, the Mesa (libglvnd) libraries on
// linux. Compiled into the app executable (it includes the generated stub list), not the platform library.
//
// Why: the firmware ships the GC2000 user-space driver in window-system flavours (-fb, -x11, -dfb, -wl) that share
// sonames, and the unsuffixed libEGL.so.1/libGLESv2.so.2 point at -fb. JUCE 9's OpenGLContext needs -x11 (EGL on an
// X11 window) and a renderer may use -fb; linking libEGL at build time would pick -fb for both. So the app links no
// GL library: every egl*/gl*/fb* symbol the app and JUCE reference is a generated stub that jumps through
// gldispatch_table, filled by load() from exactly one flavour (mixing two in a process does not work).
//
// It also adapts an EGL 1.4 driver to JUCE 9, which expects EGL 1.5 / client extensions:
//   eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)  adds EGL_EXT_platform_base/x11 + EGL_KHR_platform_x11
//   eglGetPlatformDisplay[EXT](X11, dpy)            -> eglGetDisplay(dpy)
//   eglCreatePlatformWindowSurface[EXT](&window)    -> eglCreateWindowSurface(window)
//   eglCreateContext with MAJOR/MINOR_VERSION      retried with EGL_CONTEXT_CLIENT_VERSION only
// when the driver lacks the 1.5 entry points (the real ones are used when present).
//
// On the Linux desktop (x86-64) the same stubs exist (tools/gen-gl-dispatch.py --arch x86_64: `jmp *table(%rip)`), so
// the host app links no GL library either; the "mesa" flavour loads libEGL.so.1 + libGLESv2.so.2 (no GAL). One
// flavour per process stays the rule.
#pragma once

#include <string>

namespace gldispatch
{
// Loads libGAL-<flavour>.so, libEGL-<flavour>.so, libGLESv2-<flavour>.so.2.0.0 and binds every stub. flavour: "fb" or
// "x11". A second call with another flavour fails (flavours must not be mixed). Returns false with a reason.
bool load(const char* flavour, std::string& error);
// The libraries of a flavour, loaded in this order with RTLD_GLOBAL; gal may be null (Mesa has none).
struct Libraries
{
    const char* gal;
    const char* egl;
    const char* gles;
};
inline constexpr Libraries kMesa {nullptr, "libEGL.so.1", "libGLESv2.so.2"};
// Loads the given libraries under the name `flavour` (≤ 7 characters, e.g. "mesa"). Same one-flavour rule as load().
bool loadLibraries(const char* flavour, const Libraries&, std::string& error);
const char* loadedFlavour() noexcept; // nullptr until load() succeeded
int missingCount() noexcept;          // stubs the driver does not provide (they return 0 and log once)
std::string report();                 // one line: flavour, bound/missing counts, EGL 1.5 fallbacks in use
} // namespace gldispatch
