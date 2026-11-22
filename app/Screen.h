// The demo screen, in freesp16's design (app/Theme.h): a juce::Component tree laid out on freesp16's 1280 × 768 lpx
// canvas and scaled to the 800 × 480 LCD.
//
//   status row    APP TEMPLATE › DEMO · the last control or MIDI event · renderer, fps, DSP · IN OUT ● ▶
//   context bar   ↩ (quit) · BPM | OCTAVE · TONE | JACK · SEQUENCER · the four pad scales
//   body          4 × 4 pad tiles (13–16 top … 1–4 bottom, as on the unit) · OUTPUT scope (drawn by the GLES2 scene)
//                 · OUTPUTS meters per jack · MASTER meter
//   knob strip    SOUND: VOLUME, WAVE, ATTACK, RELEASE, CUTOFF, TONE (knobs 1–6)
//
// GlDisplay paints it with JUCE's software renderer into a texture over its OpenGL ES scene, and only where something
// called repaint(). The scope's plot area stays transparent: the GL scene draws the trace there.
//
// Input: handle() takes the platform's control events (panel, strip, LCD touch; keyboard and mouse on a desktop).
// Main thread only.
#pragma once

#include "app/Midi.h"
#include "app/Synth.h"

#include "controls/LampRules.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace app
{
// What the GL scene needs from the screen each frame (app/GlDisplay.h), in LCD pixels (800 × 480).
struct SceneInput
{
    double nowMs = 0;
    juce::Rectangle<float> scope;   // the scope's plot area
    juce::Colour background, trace; // what the scene clears to, the trace colour
};

class Screen final : public juce::Component
{
public:
    static constexpr int kWidth = 800, kHeight = 480;

    Screen(Synth& synth, Midi& midi);
    ~Screen() override;

    struct Status
    {
        std::string platform, renderer, audio;
    };
    void setStatus(const Status&);

    bool handle(const io::ControlEvent& e, double nowMs); // false = not used by the demo
    void midiIn(const MidiMessageIn&, double nowMs);
    void frame(double nowMs, float fps);                   // once per frame, before the renderer draws
    void lamps(io::LampFrame&, double nowMs) const;        // the panel's lamps for this frame
    SceneInput scene(double nowMs) const;

    std::function<void()> onQuit;                          // SHIFT + BACK, or ↩ on the screen


private:
    struct Canvas;
    struct StatusRow;
    struct ContextBar;
    struct Tiles;
    struct ScopeCard;
    struct OutputsCard;
    struct MasterMeter;
    struct KnobStrip;

    void tap(juce::Point<float> lpx);
    void padDown(int pad0, int velocity, double nowMs, bool trigger); // trigger: also send the hit to the synth
    void padUp(int pad0, double nowMs);
    void setScale(int scale);
    void sendMidi(const std::uint8_t* bytes, int size, double nowMs);

    Synth& synth_;
    Midi& midi_;
    std::unique_ptr<Canvas> canvas_;
    StatusRow* status_ = nullptr;
    ContextBar* context_ = nullptr;
    Tiles* tiles_ = nullptr;
    ScopeCard* scope_ = nullptr;
    OutputsCard* outputs_ = nullptr;
    MasterMeter* master_ = nullptr;
    KnobStrip* knobs_ = nullptr;
    bool shift_ = false;
    int scale_ = 0, octave_ = 0, bpmPreset_ = 1, touchPad_ = -1, strip_ = -1;
    double lastFrameMs_ = 0;
    juce::String statusBase_; // platform · renderer
};
} // namespace app
