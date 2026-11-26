#include "app/GlDisplay.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace app
{
namespace
{
constexpr float kW = Screen::kWidth, kH = Screen::kHeight;

// Coloured triangles in screen pixels (0,0 = top left).
const char* const kVertexPixels = R"(
attribute vec2 aPos;
attribute vec4 aColour;
uniform vec2 uScreen;
varying vec4 vColour;
void main() {
    vColour = aColour;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";
const char* const kFragmentFlat = R"(
precision mediump float;
varying vec4 vColour;
void main() { gl_FragColor = vColour; }
)";

// The UI texture over the whole screen. It holds JUCE's premultiplied ARGB words, i.e. bytes B, G, R, A: swizzled.
const char* const kVertexFull = R"(
attribute vec2 aPos;
varying vec2 vUv;
void main() {
    vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";
const char* const kFragmentUi = R"(
precision mediump float;
varying vec2 vUv;
uniform sampler2D uUi;
void main() { gl_FragColor = texture2D(uUi, vUv).bgra; }
)";

GLuint compile(GLenum type, const char* source, std::string& error)
{
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (! ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
    }
    return s;
}

GLuint link(const char* vs, const char* fs, std::string& error)
{
    const GLuint v = compile(GL_VERTEX_SHADER, vs, error), f = compile(GL_FRAGMENT_SHADER, fs, error);
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aColour");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (! ok && error.empty())
        error = "the shader program does not link";
    return ok ? p : 0;
}

struct Vertex
{
    float x, y, r, g, b, a;
};

// After the app quits, the framebuffer may still show its second buffer: pan back so whatever runs next (the boot menu)
// starts from a known state.
void panFramebufferBack()
{
    const int fd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    fb_var_screeninfo v {};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && (v.xoffset != 0 || v.yoffset != 0))
    {
        v.xoffset = v.yoffset = 0;
        ioctl(fd, FBIOPAN_DISPLAY, &v);
    }
    ::close(fd);
}
} // namespace

struct GlDisplay::Impl
{
    platform::RendererEnv env;
    std::shared_ptr<void> window; // keeps the native window open
    std::string flavour, info;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLint width = 0, height = 0;
    GLuint flat = 0, ui = 0, texture = 0;
    GLint flatScreen = -1, uiSampler = -1;
    juce::Component* root = nullptr;
    DirtyAreas* dirty = nullptr; // owned by the root component
    juce::Image image {juce::Image::ARGB, Screen::kWidth, Screen::kHeight, true, juce::SoftwareImageType()};
    std::vector<Vertex> vertices;

    bool fail(std::string& error, const char* what)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s failed (EGL error 0x%x)", what, static_cast<unsigned>(eglGetError()));
        error = buf;
        return false;
    }

    // 1 + 2: repaint the dirty areas into the image and send the changed rows (full-width bands: GLES2 cannot upload
    // a sub-rectangle of a wider image) to the texture.
    void updateUi()
    {
        const auto areas = repaintUi(*root, *dirty, image);
        if (areas.isEmpty())
            return;
        const juce::Image::BitmapData bits(image, juce::Image::BitmapData::readOnly);
        juce::RectangleList<int> rows;
        for (const auto& r : areas)
            rows.add({0, r.getY(), Screen::kWidth, r.getHeight()});
        rows.consolidate();
        glBindTexture(GL_TEXTURE_2D, texture);
        for (const auto& r : rows)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r.getY(), Screen::kWidth, r.getHeight(), GL_RGBA, GL_UNSIGNED_BYTE,
                            bits.getLinePointer(r.getY())); // rows are contiguous: lineStride = width × 4
    }

    // 3: the scope, a triangle strip along the trace, offset along each point's normal; a soft wide pass, then the
    // core line. Quiet signals are magnified up to 4× so a single soft note stays visible.
    void drawScope(const SceneInput& s, const float* scope, int n)
    {
        const auto box = s.scope.reduced(0, 4);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max(peak, std::fabs(scope[i]));
        const float mid = box.getCentreY(), amp = box.getHeight() * 0.5f / std::max(0.25f, std::min(1.0f, peak));
        const auto y = [&](int k) { return mid - amp * scope[std::clamp(k, 0, n - 1)]; };
        glUseProgram(flat);
        glUniform2f(flatScreen, kW, kH);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (const auto& [halfWidth, alpha] : {std::pair {3.0f, 0.16f}, std::pair {0.9f, 1.0f}})
        {
            vertices.clear();
            const float r = s.trace.getFloatRed() * alpha, g = s.trace.getFloatGreen() * alpha, b = s.trace.getFloatBlue() * alpha;
            for (int i = 0; i < n; ++i)
            {
                const float x = box.getX() + box.getWidth() * static_cast<float>(i) / static_cast<float>(n - 1);
                const float dx = 2.0f * box.getWidth() / static_cast<float>(n - 1), dy = y(i + 1) - y(i - 1);
                const float len = std::sqrt(dx * dx + dy * dy), nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
                vertices.push_back({x + nx, y(i) + ny, r, g, b, alpha});
                vertices.push_back({x - nx, y(i) - ny, r, g, b, alpha});
            }
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].x);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].r);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(vertices.size()));
        }
        glDisableVertexAttribArray(1);
    }
};

GlDisplay::GlDisplay() : impl_(std::make_unique<Impl>()) {}

GlDisplay::~GlDisplay()
{
    auto& d = *impl_;
    if (d.root != nullptr)
        d.root->setCachedComponentImage(nullptr);
    if (d.display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(d.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (d.context != EGL_NO_CONTEXT)
            eglDestroyContext(d.display, d.context);
        if (d.surface != EGL_NO_SURFACE)
            eglDestroySurface(d.display, d.surface);
        eglTerminate(d.display);
    }
    if (d.flavour == "fb")
        panFramebufferBack();
}

bool GlDisplay::open(const platform::RendererEnv& env, std::string_view flavour, juce::Component& ui, std::string& error)
{
    auto& d = *impl_;
    d.env = env;
    d.flavour = flavour;
    if (! env.loadGl || ! env.loadGl(flavour, error))
        return error = error.empty() ? "no GL on this platform" : error, false;
    const auto window = env.nativeWindow(flavour, error);
    if (! window)
        return false;
    d.window = window->owner;

    // EGL: an RGB565-or-better window surface, an OpenGL ES 2 context, vsync.
    EGLint major = 0, minor = 0;
    d.display = eglGetDisplay((EGLNativeDisplayType) window->display);
    if (d.display == EGL_NO_DISPLAY || ! eglInitialize(d.display, &major, &minor))
        return d.fail(error, "eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 5,
                                         EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (! eglChooseConfig(d.display, configAttrs, &config, 1, &count) || count < 1)
        return d.fail(error, "eglChooseConfig");
    // EGLNativeWindowType is an integer (X11 Window) or a pointer (the Vivante fb window), depending on the headers.
    d.surface = eglCreateWindowSurface(d.display, config, (EGLNativeWindowType) window->window, nullptr);
    if (d.surface == EGL_NO_SURFACE)
        return d.fail(error, "eglCreateWindowSurface");
    static const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    d.context = eglCreateContext(d.display, config, EGL_NO_CONTEXT, contextAttrs);
    if (d.context == EGL_NO_CONTEXT || ! eglMakeCurrent(d.display, d.surface, d.surface, d.context))
        return d.fail(error, "eglCreateContext");
    eglSwapInterval(d.display, 1);
    eglQuerySurface(d.display, d.surface, EGL_WIDTH, &d.width);
    eglQuerySurface(d.display, d.surface, EGL_HEIGHT, &d.height);

    // Shaders and the UI texture (non-power-of-two is fine with clamp-to-edge and no mipmaps).
    d.flat = link(kVertexPixels, kFragmentFlat, error);
    d.ui = link(kVertexFull, kFragmentUi, error);
    if (d.flat == 0 || d.ui == 0)
        return false;
    d.flatScreen = glGetUniformLocation(d.flat, "uScreen");
    d.uiSampler = glGetUniformLocation(d.ui, "uUi");
    glGenTextures(1, &d.texture);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    for (auto [k, v] : {std::pair {GL_TEXTURE_MIN_FILTER, GL_LINEAR}, std::pair {GL_TEXTURE_MAG_FILTER, GL_LINEAR},
                        std::pair {GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE}, std::pair {GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE}})
        glTexParameteri(GL_TEXTURE_2D, k, v);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Screen::kWidth, Screen::kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    d.vertices.reserve(1024);

    d.root = &ui;
    d.dirty = attachUi(ui);

    char text[160];
    std::snprintf(text, sizeof text, "%s EGL %d.%d %s, surface %dx%d, %s, %s", d.flavour.c_str(), major, minor,
                  eglQueryString(d.display, EGL_VENDOR), d.width, d.height, reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                  window->info.c_str());
    d.info = text;
    return true;
}

bool GlDisplay::draw(const SceneInput& s, const float* scope, int scopeSize)
{
    auto& d = *impl_;
    d.updateUi();
    // The 800 × 480 picture, scaled to fit the surface (the unit: 1:1; a desktop window may be larger).
    const float k = std::min(static_cast<float>(d.width) / kW, static_cast<float>(d.height) / kH);
    const int w = static_cast<int>(kW * k), h = static_cast<int>(kH * k);
    glViewport(0, 0, d.width, d.height);
    glClearColor(s.background.getFloatRed(), s.background.getFloatGreen(), s.background.getFloatBlue(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((d.width - w) / 2, (d.height - h) / 2, w, h);
    glEnable(GL_BLEND);
    if (scopeSize > 1)
        d.drawScope(s, scope, scopeSize);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(d.ui);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    glUniform1i(d.uiSampler, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return eglSwapBuffers(d.display, d.surface) != 0; // waits for vsync (swap interval 1)
}

std::string GlDisplay::describe() const
{
    return impl_->info + " | " + (impl_->env.glReport ? impl_->env.glReport() : std::string());
}
} // namespace app
