#include "common/GlDispatch.h"


#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
const char* const symbolNames[] = {
#include "gldispatch_names.inc"
};
constexpr std::size_t symbolCount = sizeof(symbolNames) / sizeof(symbolNames[0]);

int missingSymbol()
{
    static bool logged = false;
    if (! logged)
    {
        std::fprintf(stderr, "gldispatch: a GL/EGL function the driver does not provide was called (returns 0)\n");
        logged = true;
    }
    return 0;
}

struct Driver
{
    char flavour[8] = {}; // a copy of the loaded flavour ("" = none): callers may pass a temporary's c_str()
    void* egl = nullptr;
    void* gles = nullptr;
    int missing = 0;
    bool fallbackPlatformDisplay = false, fallbackPlatformSurface = false, extendedClientExtensions = false,
         retriedContext = false;
    // Driver entry points the overrides need (resolved directly, never through the stubs).
    EGLDisplay (*getDisplay)(EGLNativeDisplayType) = nullptr;
    EGLSurface (*createWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;
    const char* (*queryString)(EGLDisplay, EGLint) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    void* (*getProcAddress)(const char*) = nullptr;
    EGLDisplay (*getPlatformDisplay)(EGLenum, void*, const EGLAttrib*) = nullptr;
    EGLDisplay (*getPlatformDisplayExt)(EGLenum, void*, const EGLint*) = nullptr;
    EGLSurface (*createPlatformWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLAttrib*) = nullptr;
    EGLSurface (*createPlatformWindowSurfaceExt)(EGLDisplay, EGLConfig, void*, const EGLint*) = nullptr;
} driver;

template <typename Fn>
void bind(Fn& fn, const char* name)
{
    void* p = dlsym(driver.egl, name);
    std::memcpy(&fn, &p, sizeof p);
}

bool isX11Platform(EGLenum platform) { return platform == EGL_PLATFORM_X11_KHR || platform == EGL_PLATFORM_X11_EXT; }

std::vector<EGLint> toIntAttribs(const EGLAttrib* attribs)
{
    std::vector<EGLint> out;
    for (; attribs != nullptr && *attribs != EGL_NONE; attribs += 2)
    {
        out.push_back(static_cast<EGLint>(attribs[0]));
        out.push_back(static_cast<EGLint>(attribs[1]));
    }
    out.push_back(EGL_NONE);
    return out;
}
} // namespace

extern "C" {
// The table the generated stubs jump through. Every slot starts at missingSymbol, so a call before load() is harmless.
void* gldispatch_table[symbolCount > 0 ? symbolCount : 1];
}

namespace
{
struct TableInit
{
    TableInit()
    {
        for (auto& slot : gldispatch_table)
            slot = reinterpret_cast<void*>(&missingSymbol);
    }
} tableInit;
} // namespace

namespace gldispatch
{
bool load(const char* flavour, std::string& error)
{
    char gal[48], egl[48], gles[48];
    std::snprintf(gal, sizeof gal, "libGAL-%s.so", flavour);
    std::snprintf(egl, sizeof egl, "libEGL-%s.so", flavour);
    std::snprintf(gles, sizeof gles, "libGLESv2-%s.so.2.0.0", flavour);
    return loadLibraries(flavour, {gal, egl, gles}, error);
}

bool loadLibraries(const char* flavour, const Libraries& libs, std::string& error)
{
    if (driver.flavour[0] != 0)
    {
        if (std::strcmp(driver.flavour, flavour) == 0)
            return true;
        error = std::string("gldispatch: flavour ") + driver.flavour + " already loaded; never mix with " + flavour;
        return false;
    }
    if (flavour[0] == 0 || std::strlen(flavour) >= sizeof driver.flavour)
        return error = std::string("gldispatch: bad flavour '") + flavour + "'", false;
    const char* gal = libs.gal;
    const char* egl = libs.egl;
    const char* gles = libs.gles;
    // Order matters: libGAL, libEGL, libGLESv2, RTLD_GLOBAL, so each library's DT_NEEDED binds to this flavour.
    // They stay loaded for the life of the process.
    if (gal != nullptr && dlopen(gal, RTLD_NOW | RTLD_GLOBAL) == nullptr)
        return error = std::string("dlopen ") + gal + ": " + dlerror(), false;
    driver.egl = dlopen(egl, RTLD_NOW | RTLD_GLOBAL);
    if (driver.egl == nullptr)
        return error = std::string("dlopen ") + egl + ": " + dlerror(), false;
    driver.gles = dlopen(gles, RTLD_NOW | RTLD_GLOBAL);
    if (driver.gles == nullptr)
        return error = std::string("dlopen ") + gles + ": " + dlerror(), false;

    bind(driver.getDisplay, "eglGetDisplay");
    bind(driver.createWindowSurface, "eglCreateWindowSurface");
    bind(driver.queryString, "eglQueryString");
    bind(driver.createContext, "eglCreateContext");
    bind(driver.getProcAddress, "eglGetProcAddress");
    bind(driver.getPlatformDisplay, "eglGetPlatformDisplay");
    bind(driver.getPlatformDisplayExt, "eglGetPlatformDisplayEXT");
    bind(driver.createPlatformWindowSurface, "eglCreatePlatformWindowSurface");
    bind(driver.createPlatformWindowSurfaceExt, "eglCreatePlatformWindowSurfaceEXT");
    if (driver.getDisplay == nullptr || driver.createWindowSurface == nullptr || driver.queryString == nullptr
        || driver.createContext == nullptr)
        return error = std::string(egl) + " lacks core EGL 1.4 entry points", false;

    driver.missing = 0;
    for (std::size_t i = 0; i < symbolCount; ++i)
    {
        void* p = dlsym(driver.gles, symbolNames[i]);
        if (p == nullptr)
            p = dlsym(driver.egl, symbolNames[i]);
        if (p == nullptr && driver.getProcAddress != nullptr)
            p = driver.getProcAddress(symbolNames[i]);
        if (p == nullptr)
            ++driver.missing;
        gldispatch_table[i] = p != nullptr ? p : reinterpret_cast<void*>(&missingSymbol);
    }
    std::memcpy(driver.flavour, flavour, std::strlen(flavour) + 1);
    return true;
}

const char* loadedFlavour() noexcept { return driver.flavour[0] != 0 ? driver.flavour : nullptr; }
int missingCount() noexcept { return driver.missing; }

std::string report()
{
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "gldispatch flavour=%s symbols=%zu missing=%d fallbacks: platformDisplay=%d platformSurface=%d "
                  "clientExtensions=%d contextRetry=%d",
                  driver.flavour[0] != 0 ? driver.flavour : "none", symbolCount, driver.missing, driver.fallbackPlatformDisplay,
                  driver.fallbackPlatformSurface, driver.extendedClientExtensions, driver.retriedContext);
    return buf;
}
} // namespace gldispatch


// ---- EGL overrides (real signatures; everything else goes through the generated stubs) ------------------------------
extern "C" {
EGLAPI const char* EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
    if (driver.queryString == nullptr)
        return nullptr;
    const char* r = driver.queryString(dpy, name);
    if (dpy != EGL_NO_DISPLAY || name != EGL_EXTENSIONS)
        return r;
    if (r != nullptr && std::strstr(r, "EGL_EXT_platform_x11") != nullptr && driver.getPlatformDisplay != nullptr)
        return r;
    // EGL 1.4 driver: advertise the X11 platform we emulate below (needed by JUCE 9's Linux OpenGLContext).
    static std::string extended;
    extended = std::string(r != nullptr ? r : "") + " EGL_EXT_client_extensions EGL_EXT_platform_base EGL_EXT_platform_x11"
                                                    " EGL_KHR_platform_x11";
    driver.extendedClientExtensions = true;
    return extended.c_str();
}

#if defined(__x86_64__)
// The Linux desktop: the app calls eglGetDisplay(X11 Display*). libglvnd guesses the platform of a native display
// pointer and, with WAYLAND_DISPLAY set (a Wayland session with XWayland), may take it for a wl_display and crash.
// The display always comes from the X11 window here, so ask for the X11 platform explicitly (EGL 1.5 / EXT entry);
// EGL_DEFAULT_DISPLAY and drivers without the entry points keep the plain call. Not on the device (ARM): there the
// generated stub binds eglGetDisplay straight to the Vivante driver.
EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType nativeDisplay)
{
    void* native = reinterpret_cast<void*>(nativeDisplay);
    if (native != nullptr && driver.getPlatformDisplay != nullptr)
        return driver.getPlatformDisplay(EGL_PLATFORM_X11_KHR, native, nullptr);
    if (native != nullptr && driver.getPlatformDisplayExt != nullptr)
        return driver.getPlatformDisplayExt(EGL_PLATFORM_X11_EXT, native, nullptr);
    return driver.getDisplay != nullptr ? driver.getDisplay(nativeDisplay) : EGL_NO_DISPLAY;
}
#endif

EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform, void* nativeDisplay, const EGLAttrib* attribs)
{
    if (driver.getPlatformDisplay != nullptr)
        return driver.getPlatformDisplay(platform, nativeDisplay, attribs);
    if (driver.getPlatformDisplayExt != nullptr)
    {
        const auto a = toIntAttribs(attribs);
        return driver.getPlatformDisplayExt(platform, nativeDisplay, a.data());
    }
    if (! isX11Platform(platform) || driver.getDisplay == nullptr)
        return EGL_NO_DISPLAY;
    driver.fallbackPlatformDisplay = true;
    return driver.getDisplay(reinterpret_cast<EGLNativeDisplayType>(nativeDisplay));
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplayEXT(EGLenum platform, void* nativeDisplay, const EGLint* attribs)
{
    if (driver.getPlatformDisplayExt != nullptr)
        return driver.getPlatformDisplayExt(platform, nativeDisplay, attribs);
    return eglGetPlatformDisplay(platform, nativeDisplay, nullptr);
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* nativeWindow,
                                                              const EGLAttrib* attribs)
{
    if (driver.createPlatformWindowSurface != nullptr)
        return driver.createPlatformWindowSurface(dpy, config, nativeWindow, attribs);
    const auto a = toIntAttribs(attribs);
    if (driver.createPlatformWindowSurfaceExt != nullptr)
        return driver.createPlatformWindowSurfaceExt(dpy, config, nativeWindow, a.data());
    // X11 platform: native_window points at the X Window id; EGL 1.4 takes the id itself.
    driver.fallbackPlatformSurface = true;
    const auto window = *static_cast<const unsigned long*>(nativeWindow);
    return driver.createWindowSurface(dpy, config, reinterpret_cast<EGLNativeWindowType>(window), a.data());
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* nativeWindow,
                                                                 const EGLint* attribs)
{
    if (driver.createPlatformWindowSurfaceExt != nullptr)
        return driver.createPlatformWindowSurfaceExt(dpy, config, nativeWindow, attribs);
    std::vector<EGLAttrib> a;
    for (; attribs != nullptr && *attribs != EGL_NONE; attribs += 2)
        a.insert(a.end(), {attribs[0], attribs[1]});
    a.push_back(EGL_NONE);
    return eglCreatePlatformWindowSurface(dpy, config, nativeWindow, a.data());
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint* attribs)
{
    if (driver.createContext == nullptr)
        return EGL_NO_CONTEXT;
    EGLContext ctx = driver.createContext(dpy, config, share, attribs);
    if (ctx != EGL_NO_CONTEXT || attribs == nullptr)
        return ctx;
    // EGL 1.4 without EGL_KHR_create_context rejects MINOR_VERSION / FLAGS. EGL_CONTEXT_MAJOR_VERSION has the value of
    // EGL_CONTEXT_CLIENT_VERSION (0x3098), so keeping only that attribute asks for the same ES major version.
    std::vector<EGLint> reduced;
    for (const EGLint* a = attribs; *a != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
            reduced.insert(reduced.end(), {a[0], a[1]});
    reduced.push_back(EGL_NONE);
    driver.retriedContext = true;
    return driver.createContext(dpy, config, share, reduced.data());
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char* name)
{
    void* p = nullptr;
    if (std::strcmp(name, "eglGetPlatformDisplay") == 0)
        p = reinterpret_cast<void*>(&eglGetPlatformDisplay);
    else if (std::strcmp(name, "eglCreatePlatformWindowSurface") == 0)
        p = reinterpret_cast<void*>(&eglCreatePlatformWindowSurface);
    else if (driver.getProcAddress != nullptr)
        p = driver.getProcAddress(name);
    if (p == nullptr && driver.gles != nullptr)
        p = dlsym(driver.gles, name);
    if (p == nullptr && driver.egl != nullptr)
        p = dlsym(driver.egl, name);
    __eglMustCastToProperFunctionPointerType fn = nullptr;
    std::memcpy(&fn, &p, sizeof p);
    return fn;
}
} // extern "C"
