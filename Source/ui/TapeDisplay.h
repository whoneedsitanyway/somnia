#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <vector>

#include "../PluginProcessor.h"

namespace somnia
{

//==============================================================================
//  The single visual surface of Somnia.
//
//  It replaces the old cassette display and the separate loop editor: two reels
//  frame a map of the whole recorded tape, the loop window is edited directly on
//  that map, and the transport / wear / pass information lives in the same
//  panel.  The tape map is the instrument's main feedback: you can see how much
//  material exists, where the loop sits inside it, and where the head is.
//==============================================================================
class TapeDisplay : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit TapeDisplay (SomniaAudioProcessor&);
    ~TapeDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    void drawReel (juce::Graphics&, juce::Point<float> centre, float radius,
                   float angle, bool active, float fill) const;
    void drawHeader (juce::Graphics&) const;
    void drawEnvelope (juce::Graphics&, juce::Rectangle<float>) const;
    void drawFooter (juce::Graphics&) const;
    void drawWave (juce::Graphics&) const;
    void rebuildWaveImage() const;

    float normAt (float x) const;
    float xAt (float norm) const;
    int   markerAt (float x) const;
    float minNorm() const;
    void  setBound (int which, float value);

    SomniaAudioProcessor& proc;

    // Filled from the audio thread's tape buffer during paint(), hence mutable.
    mutable std::vector<float> minV, maxV;
    static constexpr int kBuckets = 512;

    juce::Rectangle<float> headerArea, footerArea, waveArea;
    juce::Rectangle<float> leftReelArea, rightReelArea;

    int   dragMarker = -1;      // -1 none, 0 start, 1 end, 2 whole region
    float dragOffset = 0.0f;
    float dragLength = 0.0f;

    // A mouse drag fires far faster than the engine needs.  The target is
    // stored on every event and pushed to the parameters at ~60 Hz; mouseUp
    // commits the final position.  Flooding setValueNotifyingHost made the
    // audio thread re-apply the loop window on every block, which was audible
    // as stuttering and artefacts while a marker moved.
    void applyDragTarget();
    float  dragTargetNorm = 0.0f;
    bool   dragTargetValid = false;
    double lastDragPushMs = 0.0;

    float lastPos = -1.0f;
    int   lastCycles = -1;
    bool  lastRecording = false;
    bool  lastPlaying = false;
    float lastWear = -1.0f;
    float lastDropout = -1.0f;
    int   lastDirection = 0;
    float lastStart = -1.0f;
    float lastEnd   = -1.0f;
    float lastTape  = -1.0f;
    bool  lastHasContent = false;
    std::uint32_t lastWriteVersion = 0;
    float lastEnvLevel = -1.0f;
    int   lastEnvStage = -1;

    // Reel animation is integrated from the actual head movement, so the hubs
    // always turn the way the tape runs (forward, reverse, ping-pong).
    double reelAngle = 0.0;
    float  reelLastPos = -1.0f;

    // The waveform envelope is expensive to rebuild, so it is cached and only
    // refreshed when the recorded material changes.  The write version catches
    // a growing take even when the tape length is fixed (synced mode).
    float waveCachedTape = -1.0f;
    std::uint32_t waveCachedVersion = 0;
    bool  waveValid = false;

    // The recorded material is usually well below full scale, so drawing the
    // buckets in absolute units makes the wave a thin sliver in the middle of
    // the panel.  The waveform is therefore normalised to its own peak, with a
    // gain ceiling so pure hiss does not fill the whole area.
    mutable float waveScale = 1.0f;

    // Erosion contour: a slow-decaying peak hold of the waveform.  It stays
    // above the live wave while the tape loses material, so the "ghost" of what
    // has disintegrated is visible as an outline.
    mutable std::vector<float> erosionMax, erosionMin;
    mutable double lastErosionMs = 0.0;

    // Knob feedback: the last raw value of every visualised parameter, so a
    // change can be detected and announced on the tape map.  The parameter and
    // raw-value pointers are cached: looking them up by string cost 12 hash
    // lookups per timer tick and another few per paint.
    struct WatchedParam
    {
        const char* id;
        const char* label;
        const char* effect;
        int group;          // 0 = tone, 1 = time, 2 = decay
        juce::RangedAudioParameter* param = nullptr;
        std::atomic<float>* raw = nullptr;
        float last = 0.0f;
        bool initialised = false;
    };

    std::vector<WatchedParam> watchedParams;
    void initWatchedParams();
    void detectParamChange();
    void drawParamFeedback (juce::Graphics&) const;

    // Raw values used every frame by the map and its footer / header.
    std::atomic<float>* loopStartValue = nullptr;
    std::atomic<float>* loopEndValue   = nullptr;
    std::atomic<float>* sustainValue   = nullptr;

    float rawValue (const char* id) const;
    juce::RangedAudioParameter* paramFor (const char* id) const;

    // The wave as the active parameter will read it: a smoothed / stretched /
    // gated copy of the recorded buckets, drawn in the group colour while the
    // knob moves.  This is what makes "what does this knob do to the tape"
    // visible instead of only naming the knob.
    void drawWavePrediction (juce::Graphics&, float progress, juce::Colour tint) const;
    mutable std::vector<float> predMin, predMax;

    // Age of the current knob flash: 0 right after the move, 1 when it is gone.
    float pulseProgress() const;
    juce::Colour pulseTint() const;

    // A short travelling highlight plus the name of the parameter that moved.
    double paramPulseStartMs = -1.0e9;
    juce::String paramPulseText;
    juce::String paramPulseEffect;
    juce::String paramPulseId;
    int    paramPulseGroup = 0;

    // The waveform bitmap is rebuilt whenever the recorded material changes, so
    // the image is reused instead of being reallocated: at 30 Hz a fresh ARGB
    // bitmap of the panel is ~800 kB of garbage per second in the UI thread.
    mutable juce::Image waveImage;
    mutable int waveImageWidth = 0;
    mutable int waveImageHeight = 0;

    // Set when the recorded material changes: the bitmap is repainted into the
    // existing allocation instead of being thrown away and reallocated.
    mutable bool waveDirty = true;

    // True once waveScale has been seeded from the first take.  Starting the
    // easing from 1.0 made the wave visibly swell for the first ~20 frames.
    mutable bool waveScaleSeeded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TapeDisplay)
};

} // namespace somnia
