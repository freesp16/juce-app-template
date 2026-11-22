#include "app/Screen.h"

#include "app/Theme.h"

#include "ProjectIdentity.h"
#include "controls/ControlNames.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace app
{
namespace
{
namespace c = theme::colour;
namespace L = theme::layout;
using theme::Align;
using theme::Text;

// Body (canvas lpx): the pad tiles, the scope and outputs cards, the MASTER meter column (freesp16's ContentMeter
// template, 03-layout-grid.md). Tiles are freesp16's 102 lpx high, narrower (4 columns beside the cards).
constexpr float kTileW = 172, kTileH = 102;
const juce::Rectangle<float> kTiles {16, L::contentTop, 4 * kTileW + 3 * theme::pad, L::contentBottom - L::contentTop};
const juce::Rectangle<float> kScopeCard {768, L::contentTop, 392, 220}, kOutputsCard {768, 388, 392, 220};
const juce::Rectangle<float> kScopePlot {784, 192, 360, 160}; // multiples of 8 lpx: whole LCD pixels at × 0.625

const juce::String kChevron = juce::String(juce::CharPointer_UTF8("\xE2\x80\xBA")); // ›

juce::String noteName(int note)
{
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1); // 60 = C4
}

double noteHz(int note) { return 440.0 * std::exp2((note - 69) / 12.0); }

// Pad scales (the context bar's four buttons = the pad-mode keys TRACK, MUTE, SLICE, SCALE).
constexpr int kScales[4][7] = {{0, 3, 5, 7, 10, -1, -1}, {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, -1, -1, -1, -1, -1, -1}};
const char* const kScaleShort[4] = {"PENT", "MAJ", "MIN", "CHR"};
const char* const kScaleNames[4] = {"PENTATONIC", "MAJOR", "MINOR", "CHROMATIC"};
constexpr float kBpmPresets[4] = {90.0f, 120.0f, 140.0f, 174.0f};
const char* const kJacks[] = {"MAIN", "3-4", "5-6", "7-8", "PHONES", "SEND"};

float toDb(float linear) { return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f; }

// A displayed meter: instant attack, release ~60 dB per 300 ms (motion.meter-release), peak held 500 ms.
struct MeterState
{
    float db[2] = {-120, -120}, peak[2] = {-120, -120};
    double peakAt[2] = {0, 0};
    bool update(const float linear[2], double nowMs, float dtS)
    {
        bool changed = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float now = toDb(linear[ch]);
            float next = std::max(now, db[ch] - dtS * 60.0f / static_cast<float>(theme::meterReleaseMs / 1000.0));
            next = next < -60.0f ? -120.0f : next;
            changed = changed || std::fabs(next - db[ch]) > 0.2f;
            db[ch] = next;
            if (now >= peak[ch] || nowMs - peakAt[ch] > theme::peakHoldMs)
            {
                changed = changed || std::fabs(peak[ch] - now) > 0.2f;
                peak[ch] = now;
                peakAt[ch] = nowMs;
            }
        }
        return changed;
    }
};

void card(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& right, juce::Colour rightInk)
{
    g.setColour(c::surface2);
    g.fillRoundedRectangle(r, theme::radiusM);
    theme::text(g, r.getX() + 16, r.getY() + 20, Align::Left, Text::Caption, c::text2, title);
    if (right.isNotEmpty())
        theme::text(g, r.getRight() - 16, r.getY() + 20, Align::Right, Text::Caption, rightInk, right);
}

template <typename T>
bool assign(T& field, const T& v)
{
    if (field == v)
        return false;
    field = v;
    return true;
}
} // namespace

// ---- the canvas: freesp16's 1280 × 768 lpx, scaled to the LCD -------------------------------------------------------

struct Screen::Canvas final : juce::Component
{
    void paint(juce::Graphics& g) override
    {
        g.excludeClipRegion(kScopePlot.toNearestInt()); // the GL scene draws the scope's plot area
        g.fillAll(c::bg);
    }
};

// ---- status row (freesp16 StatusRow: breadcrumb, notification slot, indicators) -------------------------------------

struct Screen::StatusRow final : juce::Component
{
    juce::String event, diag;
    bool playing = false, toneOn = false, litIn = false, litOut = false, litRec = false;
    double midiInAt = -1e9, midiOutAt = -1e9;

    void paint(juce::Graphics& g) override
    {
        const float cy = L::statusH / 2;
        const juce::String app = juce::String(identity::displayName).toUpperCase();
        theme::text(g, 16, cy, Align::Left, Text::Label, c::text2, app);
        float x = 16 + theme::textWidth(Text::Label, app) + 8;
        theme::text(g, x, cy, Align::Left, Text::Label, c::text3, kChevron);
        x += theme::textWidth(Text::Label, kChevron) + 8;
        theme::text(g, x, cy, Align::Left, Text::Label, c::text1, "DEMO");
        theme::text(g, 320, cy, Align::Left, Text::Caption, c::text2, event);
        // Indicators right to left: PLAY (the sequencer), REC (here: the test tone), OUT, IN; diagnostics before them.
        float ix = 1264;
        theme::glyph(g, theme::Glyph::Play, ix - 9, cy - 5, 10, playing ? c::play : c::text3);
        ix -= 40;
        theme::glyph(g, theme::Glyph::Dot, ix - 9, cy - 4.5f, 9, litRec ? c::rec : c::text3);
        ix -= 40;
        theme::text(g, ix, cy, Align::Right, Text::Caption, litOut ? c::accent : c::text3, "OUT");
        ix -= 40;
        theme::text(g, ix, cy, Align::Right, Text::Caption, litIn ? c::accent : c::text3, "IN");
        theme::text(g, ix - 56, cy, Align::Right, Text::Caption, c::text3, diag);
    }
    void update(double nowMs)
    {
        const bool in = nowMs - midiInAt < theme::midiActivityMs, out = nowMs - midiOutAt < theme::midiActivityMs;
        const bool rec = toneOn && std::fmod(nowMs, theme::blinkSlowMs) < theme::blinkSlowMs / 2;
        if (assign(litIn, in) | assign(litOut, out) | assign(litRec, rec))
            repaint(1120, 0, 160, static_cast<int>(L::statusH));
    }
    void set(juce::String& field, const juce::String& v)
    {
        if (assign(field, v))
            repaint();
    }
};

// ---- context bar (freesp16 ContextBar: slot A, two split items, the readout, four menu buttons) --------------------

struct Screen::ContextBar final : juce::Component
{
    juce::String bpm, octave, tone, jack, seq;
    bool toneOn = false, seqRunning = false;
    int scale = 0;

    void paint(juce::Graphics& g) override
    {
        g.addTransform(juce::AffineTransform::translation(0, -L::contextY)); // draw in canvas coordinates
        g.setColour(c::surface1);
        g.fillRect(0.0f, L::contextY, theme::kCanvasW, L::contextH);
        theme::hline(g, 0, theme::kCanvasW, L::contextY + L::contextH, c::border);
        g.setColour(c::surface2);
        g.fillRoundedRectangle(L::slotA, theme::radiusM);
        theme::glyph(g, theme::Glyph::Back, L::slotA.getX() + 34, L::slotA.getY() + 22, 24, c::text1);
        split(g, L::slotB, "BPM", bpm, "OCTAVE", octave, false);
        split(g, L::slotC, "TONE", tone, "JACK", jack, toneOn);
        theme::text(g, L::readoutX, L::captionY, Align::Left, Text::Caption, c::text2, "SEQUENCER");
        theme::text(g, L::readoutX, L::valueY, Align::Left, Text::Value, seqRunning ? c::play : c::text1, seq);
        for (int i = 0; i < 4; ++i)
        {
            const auto r = L::menuButton(i);
            const bool sel = i == scale;
            item(g, r, sel);
            theme::text(g, r.getCentreX(), r.getY() + 26, Align::Centre, Text::ValueS, sel ? c::accent : c::text1, kScaleShort[i]);
            theme::text(g, r.getCentreX(), r.getY() + 56, Align::Centre, Text::Caption, sel ? c::accent : c::text2, kScaleNames[i]);
        }
    }
    static void item(juce::Graphics& g, juce::Rectangle<float> r, bool selected)
    {
        g.setColour(selected ? c::accentWeak : c::surface2);
        g.fillRoundedRectangle(r, theme::radiusM);
        if (selected) // the 3 lpx accent top bar
        {
            g.setColour(c::accent);
            g.fillRoundedRectangle(r.getX() + 16, r.getY(), r.getWidth() - 32, 3, 1);
        }
    }
    static void split(juce::Graphics& g, juce::Rectangle<float> r, const char* capA, const juce::String& valA,
                      const char* capB, const juce::String& valB, bool selected)
    {
        item(g, r, selected);
        theme::vline(g, r.getCentreX(), r.getY() + 12, r.getY() + 60, c::borderStrong);
        for (int i = 0; i < 2; ++i)
        {
            const float cx = r.getX() + r.getWidth() / 4 + r.getWidth() / 2 * static_cast<float>(i);
            theme::text(g, cx, L::captionY, Align::Centre, Text::Caption, c::text2, i == 0 ? capA : capB);
            theme::text(g, cx, L::valueY, Align::Centre, Text::Value, c::text1, i == 0 ? valA : valB);
        }
    }
    template <typename T>
    void set(T& field, const T& v)
    {
        if (assign(field, v))
            repaint();
    }
};

// ---- pad tiles (freesp16 TrackTile) ----------------------------------------------------------------------------------

struct Screen::Tiles final : juce::Component
{
    struct Pad
    {
        bool held = false;
        int velocity = 0, pressure = 0, note = 48;
        double hitMs = -1e9;
    };
    std::array<Pad, 16> pad;
    double now = 0;

    // In this component's coordinates (lpx from the grid's top left). Pads 1–4 at the bottom, 13–16 at the top.
    juce::Rectangle<float> cell(int pad0) const
    {
        const int row = 3 - pad0 / 4, col = pad0 % 4;
        return {(kTileW + theme::pad) * static_cast<float>(col), (kTileH + theme::pad) * static_cast<float>(row), kTileW, kTileH};
    }
    int hit(juce::Point<float> p) const
    {
        for (int i = 0; i < 16; ++i)
            if (cell(i).contains(p))
                return i;
        return -1;
    }
    float flash(int pad0) const
    {
        return static_cast<float>(std::clamp(1.0 - (now - pad[static_cast<std::size_t>(pad0)].hitMs) / theme::padFlashMs, 0.0, 1.0));
    }
    void changed(int pad0) { repaint(cell(pad0).expanded(2).toNearestInt()); }
    void frame(double nowMs)
    {
        now = nowMs;
        for (int i = 0; i < 16; ++i)
            if (nowMs - pad[static_cast<std::size_t>(i)].hitMs < theme::padFlashMs + 40)
                changed(i);
    }
    void paint(juce::Graphics& g) override
    {
        for (int i = 0; i < 16; ++i)
        {
            const auto b = cell(i);
            if (! g.clipRegionIntersects(b.toNearestInt().expanded(2)))
                continue;
            const auto& p = pad[static_cast<std::size_t>(i)];
            const auto colour = theme::track(i + 1);
            g.setColour(c::surface2);
            g.fillRoundedRectangle(b, theme::radiusM);
            if (const float f = flash(i); f > 0) // a hit: the fill flashes to the track colour at 30 % and decays
            {
                g.setColour(colour.withAlpha(0.3f * f));
                g.fillRoundedRectangle(b, theme::radiusM);
            }
            g.setColour(colour);
            g.fillRoundedRectangle(b.getX() + 12, b.getY() + 12, 32, 24, theme::radiusXs);
            theme::text(g, b.getX() + 28, b.getY() + 25, Align::Centre, Text::Label, c::onAccent, juce::String(i + 1));
            theme::badge(g, b.getX() + 52, b.getY() + 13, noteName(p.note), c::surface3, c::text1);
            // Five level segments: the velocity while held, raised by the pressure.
            const int segs = p.held ? std::min(5, 1 + std::max(p.velocity, p.pressure) / 26) : 0;
            for (int k = 0; k < 5; ++k)
            {
                g.setColour(k < segs ? colour : c::surface3);
                g.fillRoundedRectangle(b.getRight() - 52 + 8.0f * static_cast<float>(k), b.getY() + 19, 5, 10, 1);
            }
            char hz[24];
            std::snprintf(hz, sizeof hz, "%.1f Hz", noteHz(p.note));
            theme::text(g, b.getX() + 16, b.getY() + 70, Align::Left, Text::BodyStrong, p.held ? c::text1 : c::text2, hz);
            if (p.held)
                theme::text(g, b.getRight() - 16, b.getY() + 70, Align::Right, Text::Caption, c::text2,
                            "VEL " + juce::String(p.velocity) + (p.pressure > 0 ? " / " + juce::String(p.pressure) : ""));
            if (p.held) // one thin outline in the track colour while the pad is down
            {
                g.setColour(colour);
                g.drawRoundedRectangle(b.reduced(0.8f), theme::radiusM, 1.6f);
            }
        }
    }
};

// ---- the OUTPUT scope card -------------------------------------------------------------------------------------------

struct Screen::ScopeCard final : juce::Component
{
    int voices = 0;

    juce::Rectangle<float> plot() const { return kScopePlot - kScopeCard.getPosition(); }
    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat(), pl = plot();
        {
            juce::Graphics::ScopedSaveState s(g);
            g.excludeClipRegion(pl.toNearestInt()); // the GL scene draws the trace there (app/GlDisplay.cpp)
            card(g, r, "OUTPUT", juce::String(voices) + (voices == 1 ? " VOICE" : " VOICES"), c::text3);
        }
        // The grid, over the GL trace: the centre line and the quarters.
        theme::hline(g, pl.getX(), pl.getRight(), pl.getCentreY(), c::border);
        for (int i = 1; i < 4; ++i)
            theme::vline(g, pl.getX() + pl.getWidth() * static_cast<float>(i) / 4, pl.getY(), pl.getBottom(), c::border.withAlpha(0.6f));
    }
};

// ---- the OUTPUTS card: every jack but MAIN (that is the MASTER column) ----------------------------------------------

struct Screen::OutputsCard final : juce::Component
{
    std::array<MeterState, kOutPorts> meters {};
    std::array<bool, kOutPorts> present {};
    int tonePort = -1; // the jack the test tone goes to, −1 = off

    juce::Rectangle<float> column(int port) const // ports 1…5
    {
        return {16.0f + 72.0f * static_cast<float>(port - 1), 36, 72, static_cast<float>(getHeight()) - 36.0f};
    }
    void paint(juce::Graphics& g) override
    {
        card(g, getLocalBounds().toFloat(), "OUTPUTS",
             tonePort >= 0 ? "TONE " + kChevron + " " + kJacks[tonePort] : juce::String("TONE OFF"),
             tonePort >= 0 ? c::accent : c::text3);
        for (int p = 1; p < kOutPorts; ++p)
        {
            const auto col = column(p);
            const bool here = present[static_cast<std::size_t>(p)];
            const auto& m = meters[static_cast<std::size_t>(p)];
            theme::meterBars(g, col.getCentreX() - 10, col.getY() + 4, col.getBottom() - 30, 12, m.db, m.peak, ! here);
            theme::text(g, col.getCentreX(), col.getBottom() - 14, Align::Centre, Text::Caption,
                        p == tonePort ? c::accent : here ? c::text2 : c::text3, here ? juce::String(kJacks[p]) : juce::String("--"));
        }
    }
};

// ---- the MASTER meter column (freesp16 Meter::drawColumn) ------------------------------------------------------------

struct Screen::MasterMeter final : juce::Component
{
    MeterState meter;
    void paint(juce::Graphics& g) override
    {
        const float top = 184 - L::contentTop, bottom = 600 - L::contentTop;
        theme::text(g, 44, 160 - L::contentTop, Align::Centre, Text::Caption, c::text2, "MASTER");
        static constexpr std::pair<float, const char*> labels[] = {{3, "3"},     {0, "0"},     {-10, "-10"}, {-20, "-20"},
                                                                   {-30, "-30"}, {-40, "-40"}, {-50, "-50"}};
        for (const auto& [db, label] : labels)
            theme::text(g, 84, theme::meterY(db, top, bottom), Align::Right, Text::Caption, c::text3, label);
        theme::meterBars(g, 12, top, bottom, 12, meter.db, meter.peak, false);
    }
};

// ---- the knob strip (freesp16 KnobStrip + ParamColumn) ---------------------------------------------------------------

struct Screen::KnobStrip final : juce::Component
{
    static constexpr const char* captions[6] = {"VOLUME", "WAVE", "ATTACK", "RELEASE", "CUTOFF", "TONE"};
    std::array<float, 6> value {}; // 0…1 for the value track
    std::array<juce::String, 6> text {};
    std::array<double, 6> touchedAt {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9};
    std::array<bool, 6> underlined {};

    juce::Rectangle<int> column(int n) const
    {
        return juce::Rectangle<float>(L::columnX0 + L::columnW * static_cast<float>(n), L::tabH, L::columnW, L::stripH - L::tabH)
            .toNearestInt();
    }
    void set(int n, float v, const juce::String& t)
    {
        const auto i = static_cast<std::size_t>(n);
        if (std::fabs(value[i] - v) < 1e-4f && text[i] == t)
            return;
        value[i] = v;
        text[i] = t;
        repaint(column(n));
    }
    void frame(double nowMs)
    {
        for (int n = 0; n < 6; ++n)
            if (assign(underlined[static_cast<std::size_t>(n)], nowMs - touchedAt[static_cast<std::size_t>(n)] < theme::recentlyTouchedMs))
                repaint(column(n));
    }
    void paint(juce::Graphics& g) override
    {
        g.addTransform(juce::AffineTransform::translation(0, -L::stripY)); // draw in canvas coordinates
        g.setColour(c::surface1);
        g.fillRect(0.0f, L::stripY, theme::kCanvasW, L::stripH);
        theme::hline(g, 0, theme::kCanvasW, L::stripY, c::border);
        juce::Path tab; // the strip tab: a trapezoid, top edge 0–224, bottom 0–248
        tab.addQuadrilateral(0, L::stripY, L::tabTopW, L::stripY, L::tabW, L::stripY + L::tabH, 0, L::stripY + L::tabH);
        g.setColour(c::surface2);
        g.fillPath(tab);
        theme::text(g, 16, L::stripY + 13, Align::Left, Text::Caption, c::text2, "SOUND");
        for (int n = 0; n < 6; ++n)
        {
            const auto i = static_cast<std::size_t>(n);
            const float x = L::columnX0 + L::columnW * static_cast<float>(n), cx = x + L::columnW / 2;
            theme::text(g, cx, L::columnCaptionY, Align::Centre, Text::Caption, c::text2, captions[n]);
            theme::text(g, cx, L::columnValueY, Align::Centre, Text::Value, c::text1, text[i]);
            if (underlined[i]) // the accent underline after a change
            {
                g.setColour(c::accent);
                g.fillRoundedRectangle(x + 64, L::columnUnderlineY, 80, 2, 1);
            }
            g.setColour(c::borderStrong); // the value track, 128 × 2
            g.fillRoundedRectangle(x + 40, L::columnTrackY, 128, 2, 1);
            g.setColour(c::text2);
            g.fillRoundedRectangle(x + 40, L::columnTrackY, std::max(2.0f, 128.0f * std::clamp(value[i], 0.0f, 1.0f)), 2, 1);
        }
    }
};

// ---- the screen ------------------------------------------------------------------------------------------------------

Screen::Screen(Synth& synth, Midi& midi) : synth_(synth), midi_(midi)
{
    setOpaque(false);
    setLookAndFeel(&theme::lookAndFeel()); // inherited by every child (app/Theme.h)
    canvas_ = std::make_unique<Canvas>();
    canvas_->setInterceptsMouseClicks(false, false);
    canvas_->setBounds(0, 0, static_cast<int>(theme::kCanvasW), static_cast<int>(theme::kCanvasH));
    canvas_->setTransform(juce::AffineTransform::scale(theme::kScale));
    addAndMakeVisible(*canvas_);
    auto add = [this](auto* part, juce::Rectangle<float> bounds) {
        part->setInterceptsMouseClicks(false, false);
        part->setBounds(bounds.toNearestInt());
        canvas_->addAndMakeVisible(part);
        return part;
    };
    status_ = add(new StatusRow(), {0, 0, theme::kCanvasW, L::statusH});
    context_ = add(new ContextBar(), {0, L::contextY, theme::kCanvasW, L::contextH + 1});
    tiles_ = add(new Tiles(), kTiles);
    scope_ = add(new ScopeCard(), kScopeCard);
    outputs_ = add(new OutputsCard(), kOutputsCard);
    master_ = add(new MasterMeter(), {L::meterColumnX, L::contentTop, 88, L::contentBottom - L::contentTop});
    knobs_ = add(new KnobStrip(), {0, L::stripY, theme::kCanvasW, L::stripH});
    setScale(0);
    setSize(kWidth, kHeight);
}

Screen::~Screen()
{
    canvas_->deleteAllChildren();
    setLookAndFeel(nullptr);
}

void Screen::setStatus(const Status& s)
{
    statusBase_ = juce::String::fromUTF8((s.platform + " · " + s.renderer).c_str()).toUpperCase();
    status_->set(status_->diag, statusBase_);
}

void Screen::setScale(int scale)
{
    scale_ = std::clamp(scale, 0, 3);
    const int* degrees = kScales[scale_];
    int count = 0;
    while (count < 7 && degrees[count] >= 0)
        ++count;
    for (int i = 0; i < 16; ++i)
    {
        const int note = std::clamp(scale_ == 3 ? 48 + 12 * octave_ + i : 48 + 12 * octave_ + 12 * (i / count) + degrees[i % count], 0, 127);
        synth_.padNote[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(note);
        tiles_->pad[static_cast<std::size_t>(i)].note = note;
        tiles_->changed(i);
    }
    synth_.seqNote = synth_.padNote[0].load();
}

void Screen::sendMidi(const std::uint8_t* bytes, int size, double nowMs)
{
    midi_.send(bytes, size);
    if (midi_.outputCount() > 0)
        status_->midiOutAt = nowMs;
}

void Screen::padDown(int pad0, int velocity, double nowMs, bool trigger)
{
    if (trigger) // touch and scripted input: no reader thread sent it to the synth
        synth_.push({static_cast<std::uint8_t>(pad0), static_cast<std::uint8_t>(velocity), 60, static_cast<std::int8_t>(pad0)});
    auto& p = tiles_->pad[static_cast<std::size_t>(pad0)];
    p.held = true;
    p.velocity = velocity;
    p.pressure = 0;
    p.hitMs = nowMs;
    tiles_->now = nowMs;
    tiles_->changed(pad0);
    synth_.seqNote = p.note; // the sequencer plays the last pad
    const std::uint8_t on[3] = {0x90, static_cast<std::uint8_t>(p.note), static_cast<std::uint8_t>(velocity)};
    sendMidi(on, 3, nowMs);
}

void Screen::padUp(int pad0, double nowMs)
{
    synth_.padRelease(pad0);
    auto& p = tiles_->pad[static_cast<std::size_t>(pad0)];
    if (! p.held)
        return;
    p.held = false;
    tiles_->changed(pad0);
    const std::uint8_t off[3] = {0x80, static_cast<std::uint8_t>(p.note), 0};
    sendMidi(off, 3, nowMs);
}

// A tap on the context bar (canvas lpx): ↩ quits, the items act like the keys they stand for.
void Screen::tap(juce::Point<float> p)
{
    if (L::slotA.contains(p))
    {
        if (onQuit)
            onQuit();
    }
    else if (L::slotB.contains(p))
    {
        if (p.x < L::slotB.getCentreX())
            synth_.bpm = kBpmPresets[bpmPreset_ = (bpmPreset_ + 1) % 4];
        else
        {
            octave_ = octave_ >= 2 ? -2 : octave_ + 1;
            setScale(scale_);
        }
    }
    else if (L::slotC.contains(p))
    {
        if (p.x < L::slotC.getCentreX())
            synth_.toneOn = ! synth_.toneOn.load();
        else
            synth_.tonePort = (synth_.tonePort.load() + 1) % (kOutPorts - 1); // MAIN … PHONES
    }
    else if (L::slotD.contains(p))
        synth_.seqRunning = ! synth_.seqRunning.load();
    else
        for (int i = 0; i < 4; ++i)
            if (L::menuButton(i).contains(p))
                setScale(i);
}

bool Screen::handle(const io::ControlEvent& e, double nowMs)
{
    using K = io::ControlEvent::Kind;
    using C = io::Control;
    if (e.control != C::Screen) // the status row shows the last control event ("ENC.3 TURN -2")
    {
        char text[64];
        io::describe(e, text, sizeof text);
        status_->set(status_->event, juce::String(text).toUpperCase());
    }
    // Pads: the panel's and the keyboard's reader already sent the hit to the synth (TriggerSink).
    if (io::isPad(e.control))
    {
        const int pad0 = io::indexOf(e.control) - 1;
        const bool triggered = e.source == io::kSourcePanel || e.source == io::kSourceKeyboard;
        if (e.kind == K::Press)
            padDown(pad0, std::clamp<int>(e.value, 1, 127), nowMs, ! triggered);
        else if (e.kind == K::Release)
            padUp(pad0, nowMs);
        else if (e.kind == K::Pressure)
        {
            tiles_->pad[static_cast<std::size_t>(pad0)].pressure = e.value;
            tiles_->changed(pad0);
            if (! triggered)
                synth_.push(platform::pressureTrigger(pad0, e.value));
        }
        return true;
    }
    // LCD touch, in 800 × 480 points → canvas lpx: pads play, the context bar's items act.
    if (e.control == C::Screen && e.kind == K::Touch)
    {
        const juce::Point<float> at(static_cast<float>(e.x) / theme::kScale, static_cast<float>(e.y) / theme::kScale);
        if (e.value == 1)
        {
            const int pad0 = tiles_->hit(at - kTiles.getPosition());
            if (pad0 >= 0)
            {
                touchPad_ = pad0;
                padDown(pad0, 100, nowMs, true);
            }
            else
                tap(at);
        }
        else if (e.value == 0 && touchPad_ >= 0)
        {
            padUp(touchPad_, nowMs);
            touchPad_ = -1;
        }
        status_->set(status_->event, "SCREEN TOUCH " + juce::String(e.value) + " @" + juce::String(e.x) + "," + juce::String(e.y));
        return true;
    }
    if (e.control == C::Shift)
        shift_ = e.kind == K::Press;
    if (e.kind == K::Turn && io::isKnob(e.control))
    {
        const int d = e.value, n = io::indexOf(e.control);
        knobs_->touchedAt[static_cast<std::size_t>(n - 1)] = nowMs;
        switch (n)
        {
        case 1: synth_.volume = std::clamp(synth_.volume.load() + 0.02f * static_cast<float>(d), 0.0f, 1.0f); break;
        case 2: synth_.wave = std::clamp(synth_.wave.load() + 0.05f * static_cast<float>(d), 0.0f, 3.0f); break;
        case 3: synth_.attackMs = std::clamp(synth_.attackMs.load() * std::pow(1.12f, static_cast<float>(d)), 1.0f, 2000.0f); break;
        case 4: synth_.releaseMs = std::clamp(synth_.releaseMs.load() * std::pow(1.12f, static_cast<float>(d)), 5.0f, 4000.0f); break;
        case 5: synth_.cutoff = std::clamp(synth_.cutoff.load() + 0.01f * static_cast<float>(d), 0.0f, 1.0f); break;
        case 6:
            synth_.toneHz = std::clamp(synth_.toneHz.load() * std::exp2(static_cast<float>(d) / (shift_ ? 120.0f : 12.0f)), 20.0f, 20000.0f);
            break;
        default: break;
        }
    }
    else if (e.control == C::Selector && e.kind == K::Turn)
    {
        if (shift_)
            synth_.toneSide = (synth_.toneSide.load() + (e.value > 0 ? 1 : 2)) % 3;
        else
            synth_.tonePort = std::clamp(synth_.tonePort.load() + (e.value > 0 ? 1 : -1), 0, kOutPorts - 1);
    }
    else if (e.kind == K::Press)
    {
        switch (e.control)
        {
        case C::SelectorPush: synth_.toneOn = ! synth_.toneOn.load(); break;
        case C::Play: synth_.seqRunning = ! synth_.seqRunning.load(); break;
        case C::Stop:
            synth_.seqRunning = false;
            synth_.toneOn = false;
            synth_.allNotesOff();
            break;
        case C::Back:
            if (shift_ && onQuit)
                onQuit();
            break;
        case C::PadModeTrack: setScale(0); break;
        case C::PadModeMute: setScale(1); break;
        case C::PadModeSlice: setScale(2); break;
        case C::PadModeScale: setScale(3); break;
        case C::Scene: octave_ = std::max(-2, octave_ - 1); setScale(scale_); break;
        case C::Pattern: octave_ = std::min(2, octave_ + 1); setScale(scale_); break;
        default:
            if (io::isStep(e.control))
                synth_.seqSteps = synth_.seqSteps.load() ^ (1u << (io::indexOf(e.control) - 1));
            else if (e.control >= C::Bar1 && e.control <= C::Bar4)
            {
                bpmPreset_ = static_cast<int>(e.control) - static_cast<int>(C::Bar1);
                synth_.bpm = kBpmPresets[bpmPreset_];
            }
            break;
        }
    }
    if (e.control == C::Strip)
    {
        strip_ = e.kind == K::Release ? -1 : e.value;
        if (e.source != io::kSourceStrip) // other strips (mouse wheel, script) → the synth
            synth_.push(platform::stripTrigger(e.value, e.kind != K::Release));
    }
    return true;
}

void Screen::midiIn(const MidiMessageIn& m, double nowMs)
{
    char text[96];
    const int type = m.bytes[0] & 0xF0, ch = (m.bytes[0] & 15) + 1;
    const std::string port = m.port < midi_.ports().size() ? midi_.ports()[m.port].name : "?";
    if (type == 0x90 && m.bytes[2] > 0)
        std::snprintf(text, sizeof text, "MIDI %s CH%d NOTE ON %s %d", port.c_str(), ch, noteName(m.bytes[1]).toRawUTF8(), m.bytes[2]);
    else if (type == 0x80 || type == 0x90)
        std::snprintf(text, sizeof text, "MIDI %s CH%d NOTE OFF %s", port.c_str(), ch, noteName(m.bytes[1]).toRawUTF8());
    else if (type == 0xB0)
        std::snprintf(text, sizeof text, "MIDI %s CH%d CC %d = %d", port.c_str(), ch, m.bytes[1], m.bytes[2]);
    else if (type == 0xE0)
        std::snprintf(text, sizeof text, "MIDI %s CH%d BEND %d", port.c_str(), ch, (m.bytes[2] << 7 | m.bytes[1]) - 8192);
    else
        std::snprintf(text, sizeof text, "MIDI %s %02X %02X %02X", port.c_str(), m.bytes[0], m.bytes[1], m.size > 2 ? m.bytes[2] : 0);
    status_->set(status_->event, juce::String(text).toUpperCase());
    status_->midiInAt = nowMs;
}

void Screen::frame(double nowMs, float fps)
{
    const float dtS = lastFrameMs_ > 0 ? static_cast<float>((nowMs - lastFrameMs_) / 1000.0) : 0.016f;
    const bool halfSecond = static_cast<long>(nowMs / 500.0) != static_cast<long>(lastFrameMs_ / 500.0);
    lastFrameMs_ = nowMs;
    tiles_->frame(nowMs);
    knobs_->frame(nowMs);

    // Status row.
    status_->playing = synth_.seqRunning.load();
    status_->toneOn = synth_.toneOn.load();
    status_->update(nowMs);
    if (halfSecond)
    {
        char tail[64];
        std::snprintf(tail, sizeof tail, " · %.0f FPS · DSP %.1f%%", static_cast<double>(fps),
                      static_cast<double>(synth_.loadAverage() * 100.0f));
        status_->set(status_->diag, statusBase_ + juce::String::fromUTF8(tail)); // UTF-8: the middle dots
    }

    // Context bar.
    char t[32];
    std::snprintf(t, sizeof t, "%.1f", static_cast<double>(synth_.bpm.load()));
    context_->set(context_->bpm, juce::String(t));
    context_->set(context_->octave, juce::String(octave_ > 0 ? "+" : "") + juce::String(octave_));
    context_->set(context_->toneOn, synth_.toneOn.load());
    context_->set(context_->tone, juce::String(synth_.toneOn.load() ? "ON" : "OFF"));
    static const char* const sides[] = {"", " L", " R"};
    const int port = std::clamp(synth_.tonePort.load(), 0, kOutPorts - 1);
    context_->set(context_->jack, juce::String(port == 4 ? "PH" : kJacks[port]) + sides[std::clamp(synth_.toneSide.load(), 0, 2)]);
    const int step = synth_.step();
    context_->set(context_->seqRunning, synth_.seqRunning.load());
    std::snprintf(t, sizeof t, "STEP %02d", step + 1);
    context_->set(context_->seq, synth_.seqRunning.load() && step >= 0 ? juce::String(t) : juce::String("STOPPED"));
    context_->set(context_->scale, scale_);

    // Knob strip.
    std::snprintf(t, sizeof t, "%.0f", static_cast<double>(synth_.volume.load() * 100.0f));
    knobs_->set(0, synth_.volume.load(), t);
    static const char* const waves[] = {"SINE", "TRI", "SAW", "SQUARE"};
    const float w = synth_.wave.load();
    const int wi = static_cast<int>(std::lround(w));
    knobs_->set(1, w / 3.0f, juce::String(waves[std::clamp(wi, 0, 3)]) + (std::fabs(w - static_cast<float>(wi)) > 0.02f ? "~" : ""));
    std::snprintf(t, sizeof t, "%.0f ms", static_cast<double>(synth_.attackMs.load()));
    knobs_->set(2, std::log(synth_.attackMs.load()) / std::log(2000.0f), t);
    std::snprintf(t, sizeof t, "%.0f ms", static_cast<double>(synth_.releaseMs.load()));
    knobs_->set(3, std::log(synth_.releaseMs.load() / 5.0f) / std::log(800.0f), t);
    const float hz = 40.0f * std::exp2(9.0f * synth_.cutoff.load());
    std::snprintf(t, sizeof t, hz >= 1000.0f ? "%.1f kHz" : "%.0f Hz", static_cast<double>(hz >= 1000.0f ? hz / 1000.0f : hz));
    knobs_->set(4, synth_.cutoff.load(), t);
    std::snprintf(t, sizeof t, "%.0f Hz", static_cast<double>(synth_.toneHz.load()));
    knobs_->set(5, std::log(synth_.toneHz.load() / 20.0f) / std::log(1000.0f), t);

    // Meters: MAIN in the MASTER column, the other jacks in OUTPUTS.
    for (int p = 0; p < kOutPorts; ++p)
    {
        const float peaks[2] = {synth_.takePeak(p, 0), synth_.takePeak(p, 1)};
        if (p == 0)
        {
            if (master_->meter.update(peaks, nowMs, dtS))
                master_->repaint();
            continue;
        }
        const bool moved = outputs_->meters[static_cast<std::size_t>(p)].update(peaks, nowMs, dtS);
        if (assign(outputs_->present[static_cast<std::size_t>(p)], synth_.portPresent(p)) || moved)
            outputs_->repaint(outputs_->column(p).toNearestInt());
    }
    if (assign(outputs_->tonePort, synth_.toneOn.load() ? port : -1))
        outputs_->repaint();

    // Scope header (the GL scene draws the trace from the synth itself).
    if (assign(scope_->voices, synth_.activeVoices()))
        scope_->repaint(0, 0, scope_->getWidth(), 36);
}

void Screen::lamps(io::LampFrame& f, double) const
{
    using C = io::Control;
    using io::LampMode;
    for (int i = 0; i < 16; ++i) // pads in their track colours, as freesp16 shows them on the unit
    {
        const auto& p = tiles_->pad[static_cast<std::size_t>(i)];
        const auto led = theme::kTrack[i].led;
        f[io::pad(i + 1)] = p.held ? io::LampState {LampMode::Flash, led, p.hitMs} : io::LampState {LampMode::Dim, led, 0};
    }
    const std::uint32_t steps = synth_.seqSteps.load();
    const int step = synth_.step();
    for (int i = 0; i < 16; ++i)
    {
        const bool on = (steps >> i) & 1u;
        if (i == step)
            f[io::step(i + 1)] = {LampMode::On, io::lampcolour::white, 0};
        else
            f[io::step(i + 1)] = on ? io::LampState {LampMode::On, theme::kTrack[0].led, 0} : io::LampState {};
    }
    f[C::Play] = synth_.seqRunning.load() ? io::LampState {LampMode::On, io::lampcolour::green, 0} : io::LampState {};
    f[C::Stop] = {};
    // REC blinks while the test tone sounds. (Never drive AFS ACTIVE from a demo: on the unit that lamp also switches
    // the analog filter's select line, hw/imx6/AnalogBoard.h.)
    f[C::Rec] = synth_.toneOn.load() ? io::LampState {LampMode::BlinkSlow, io::lampcolour::red, 0} : io::LampState {};
    const C modes[] = {C::PadModeTrack, C::PadModeMute, C::PadModeSlice, C::PadModeScale};
    for (int i = 0; i < 4; ++i)
        f[modes[i]] = i == scale_ ? io::LampState {LampMode::On, io::lampcolour::white, 0} : io::LampState {};
    for (int i = 0; i < 4; ++i)
        f[static_cast<C>(static_cast<int>(C::Bar1) + i)] =
            i == bpmPreset_ ? io::LampState {LampMode::On, io::lampcolour::white, 0} : io::LampState {};
    f[C::Scene] = octave_ < 0 ? io::LampState {LampMode::On, io::lampcolour::white, 0} : io::LampState {};
    f[C::Pattern] = octave_ > 0 ? io::LampState {LampMode::On, io::lampcolour::white, 0} : io::LampState {};
    io::stripLamps(strip_ < 0 ? 0 : strip_, strip_ >= 0, false, io::StripMode::Pitch, f);
    f[C::StripModePitch] = {LampMode::On, io::lampcolour::white, 0};
}

SceneInput Screen::scene(double nowMs) const
{
    SceneInput s;
    s.nowMs = nowMs;
    s.scope = kScopePlot.transformedBy(juce::AffineTransform::scale(theme::kScale));
    s.background = c::bg;
    s.trace = c::accent;
    return s;
}
} // namespace app
