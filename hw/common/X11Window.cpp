#include "common/X11Window.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace platform::common
{

struct X11Window
{
    Display* dpy = nullptr;
    Window win = 0;
    GC gc = nullptr;
    int w = 0, h = 0, depth = 0;
    Visual* visual = nullptr;
    Atom wmDelete = 0;        // managed windows: WM_DELETE_WINDOW
    ~X11Window()
    {
        if (dpy != nullptr)
            XCloseDisplay(dpy);
    }
};

bool x11ScreenSize(int& w, int& h)
{
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr)
        return false;
    w = DisplayWidth(dpy, DefaultScreen(dpy));
    h = DisplayHeight(dpy, DefaultScreen(dpy));
    XCloseDisplay(dpy);
    return true;
}

namespace
{
// A managed window is mapped by the window manager: wait for its MapNotify so the first frame is not lost (an Expose
// later re-sends the whole frame anyway). Other events stay queued for the input reader.
void waitMapped(Display* dpy, Window win)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    XEvent ev;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (XCheckTypedWindowEvent(dpy, win, MapNotify, &ev))
            return;
        pollfd fd {ConnectionNumber(dpy), POLLIN, 0};
        ::poll(&fd, 1, 10);
    }
}
} // namespace

std::shared_ptr<X11Window> openX11Window(bool inputOnly, std::string& error)
{
    X11WindowOptions o;
    o.inputOnly = inputOnly;
    return openX11Window(o, error);
}

std::shared_ptr<X11Window> openX11Window(const X11WindowOptions& o, std::string& error)
{
    const bool inputOnly = o.inputOnly;
    auto x = std::make_shared<X11Window>();
    x->dpy = XOpenDisplay(nullptr);
    if (x->dpy == nullptr)
    {
        error = "XOpenDisplay failed (DISPLAY not set or server unreachable)";
        return nullptr;
    }
    // A TCP display (DISPLAY=host:n, e.g. a remote Xvfb) must not hold small requests back: with Nagle on, a partial
    // XPutImage + XSync waits for the server's delayed ACK (~40 ms per frame). Harmless on a Unix socket (it fails).
    {
        int one = 1;
        setsockopt(ConnectionNumber(x->dpy), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    const int screen = DefaultScreen(x->dpy);
    const Window root = RootWindow(x->dpy, screen);
    x->w = DisplayWidth(x->dpy, screen);
    x->h = DisplayHeight(x->dpy, screen);
    x->depth = DefaultDepth(x->dpy, screen);
    x->visual = DefaultVisual(x->dpy, screen);
    if (o.managed)
    {
        if (o.width > 0 && o.height > 0)
            x->w = o.width, x->h = o.height;
        XSetWindowAttributes a;
        std::memset(&a, 0, sizeof a);
        a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask
                     | ExposureMask | StructureNotifyMask | FocusChangeMask;
        a.background_pixmap = None; // X never paints over a frame
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0,
                               inputOnly ? 0 : x->depth, inputOnly ? InputOnly : InputOutput,
                               inputOnly ? CopyFromParent : x->visual,
                               CWEventMask | (inputOnly ? 0ul : static_cast<unsigned long>(CWBackPixmap)), &a);
        XSizeHints* hints = XAllocSizeHints(); // fixed size: the frame is 800 x 480 x scale, never stretched
        if (hints != nullptr)
        {
            hints->flags = PSize | PMinSize | PMaxSize;
            hints->width = hints->min_width = hints->max_width = x->w;
            hints->height = hints->min_height = hints->max_height = x->h;
            XSetWMNormalHints(x->dpy, x->win, hints);
            XFree(hints);
        }
        XStoreName(x->dpy, x->win, o.title.c_str());
        const Atom utf8 = XInternAtom(x->dpy, "UTF8_STRING", False);
        XChangeProperty(x->dpy, x->win, XInternAtom(x->dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(o.title.c_str()), static_cast<int>(o.title.size()));
        x->wmDelete = XInternAtom(x->dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(x->dpy, x->win, &x->wmDelete, 1);
        XMapWindow(x->dpy, x->win);
        if (! inputOnly)
            x->gc = XCreateGC(x->dpy, x->win, 0, nullptr);
        XSync(x->dpy, False);
        waitMapped(x->dpy, x->win);
        return x;
    }

    XSetWindowAttributes a;
    std::memset(&a, 0, sizeof a);
    a.override_redirect = True; // no window manager on the device
    a.event_mask = ButtonPressMask | ButtonReleaseMask | ButtonMotionMask;
    a.background_pixmap = None; // X never paints over a frame
    if (inputOnly)
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0, 0,
                               InputOnly, CopyFromParent, CWOverrideRedirect | CWEventMask, &a);
    else
        x->win = XCreateWindow(x->dpy, root, 0, 0, static_cast<unsigned>(x->w), static_cast<unsigned>(x->h), 0,
                               x->depth, InputOutput, x->visual, CWOverrideRedirect | CWEventMask | CWBackPixmap, &a);

    const Pixmap empty = XCreatePixmap(x->dpy, root, 1, 1, 1);
    XColor black;
    std::memset(&black, 0, sizeof black);
    const Cursor none = XCreatePixmapCursor(x->dpy, empty, empty, &black, &black, 0, 0);
    XDefineCursor(x->dpy, x->win, none);
    XFreePixmap(x->dpy, empty);
    XMapRaised(x->dpy, x->win);
    if (! inputOnly)
        x->gc = XCreateGC(x->dpy, x->win, 0, nullptr);
    XSync(x->dpy, False);
    return x;
}

void x11Size(const X11Window& x, int& w, int& h)
{
    w = x.w;
    h = x.h;
}

// Native window handles for the GLES backend (EGL x11 flavour).
void* x11Display(const X11Window& x)
{
    return x.dpy;
}
unsigned long x11Drawable(const X11Window& x)
{
    return x.win;
}
unsigned long x11WmDeleteAtom(const X11Window& x)
{
    return x.wmDelete;
}

} // namespace platform::common
