#include "TapeDisplay.h"
#include "SomniaLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace somnia
{

//==============================================================================
TapeDisplay::TapeDisplay (SomniaAudioProcessor& p)
    : proc (p)
{
    minV.assign ((size_t) kBuckets, 0.0f);
    maxV.assign ((size_t) kBuckets, 0.0f);
    predMin.assign ((size_t) kBuckets, 0.0f);
    predMax.assign ((size_t) kBuckets, 0.0f);
    erosionMax.assign ((size_t) kBuckets, 0.0f);
    erosionMin.assign ((size_t) kBuckets, 0.0f);

    initWatchedParams();

    loopStartValue = p.apvts.getRawParameterValue (ids::loopStart);
    loopEndValue   = p.apvts.getRawParameterValue (ids::loopEnd);
    sustainValue   = p.apvts.getRawParameterValue (ids::sustain);

    setTooltip ("TAPE MAP - drag the START / END markers to set the loop, "
                "drag the highlighted region to move it, double-click to reset");
    setMouseCursor (juce::MouseCursor::NormalCursor);

    // 30 Hz is enough for a tape map: the head moves smoothly, and repainting
    // a 1100 px wide panel at 60 Hz was the main cause of the sluggish UI.
    startTimerHz (30);
}

TapeDisplay::~TapeDisplay()
{
    stopTimer();
}

//==============================================================================
void TapeDisplay::resized()
{
    auto b = getLocalBounds().toFloat().reduced (12.0f, 8.0f);

    headerArea = b.removeFromTop (24.0f);
    footerArea = b.removeFromBottom (18.0f);

    b.removeFromTop (4.0f);
    b.removeFromBottom (4.0f);

    const float reelW = juce::jmin (96.0f, b.getWidth() * 0.11f);

    leftReelArea  = b.removeFromLeft (reelW);
    rightReelArea = b.removeFromRight (reelW);
    b.removeFromLeft (8.0f);
    b.removeFromRight (8.0f);

    waveArea = b;

    // The waveform bitmap depends on the panel size.
    waveImage = {};
    waveImageWidth = 0;
    waveImageHeight = 0;
}

void TapeDisplay::lookAndFeelChanged()
{
    // The scope background and the wave colour are baked into the cached
    // bitmap, so a theme switch has to throw it away.
    waveImage = {};
    waveImageWidth = 0;
    waveImageHeight = 0;
    repaint();
}

void TapeDisplay::timerCallback()
{
    // In compact mode the panel is hidden but the timer keeps running: skip the
    // whole per-frame pipeline (telemetry, parameter scan, waveform copy).  The
    // cache is marked stale so the first tick after it comes back repaints, and
    // the knob baselines are dropped so no flash fires for moves made while the
    // panel was away.
    //
    // isVisible(), not isShowing(): the latter is also false before the editor
    // reaches a desktop window, which would leave the map permanently blank in
    // an off-screen render (and in any host that shows the editor lazily).
    if (! isVisible())
    {
        lastPos = -1.0f;
        lastCycles = -1;
        lastHasContent = false;

        for (auto& wp : watchedParams)
            wp.initialised = false;

        return;
    }

    const float pos      = proc.getDisplayLoopPosition();
    const int   cycles   = proc.getDisplayCycles();
    const bool  rec      = proc.getDisplayRecording();
    const bool  play     = proc.getDisplayPlaying();
    const float wear     = proc.getDisplayWear();
    const float dropout  = proc.getDisplayDropout();
    const int   dir      = proc.getDirectionSign();
    const float tape     = proc.getDisplayTapeLengthSeconds();
    const bool  hasTape  = proc.getDisplayHasContent();
    const float start    = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
    const float end      = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
    const float envLevel = proc.getDisplayEnvelopeLevel();
    const int   envStage = proc.getDisplayEnvelopeStage();
    const auto  writeVersion = proc.getDisplayWriteVersion();

    // Knob feedback has to be sampled before the early-out below, otherwise a
    // stationary tape would never report a parameter move.
    detectParamChange();

    // The response flash keeps its own animation running for its whole life,
    // independent of the tape telemetry.
    const bool pulseActive = paramPulseStartMs > -1.0e8
                             && (juce::Time::getMillisecondCounterHiRes() - paramPulseStartMs) < 1250.0;

    // Integrate the reel angle from the real head movement.  The head position
    // wraps from 1 to 0 (and reflects in ping-pong), so unwrap the delta before
    // applying it; that keeps the hubs turning in the direction the tape runs.
    if (proc.getDisplayFreshRecording())
    {
        // Nothing is read back yet, but the tape is being wound onto the reel.
        reelAngle = std::fmod (reelAngle + juce::MathConstants<double>::twoPi * 1.1 / 30.0,
                               juce::MathConstants<double>::twoPi);
        reelLastPos = -1.0f;
    }
    else if (play || rec)
    {
        if (reelLastPos >= 0.0f)
        {
            double delta = (double) pos - (double) reelLastPos;
            if (delta > 0.5)  delta -= 1.0;
            if (delta < -0.5) delta += 1.0;

            // Rew/FF and marker drags move the head in one jump; do not let the
            // hubs spin wildly because of that.
            delta = juce::jlimit (-0.25, 0.25, delta);

            reelAngle = std::fmod (reelAngle + delta * juce::MathConstants<double>::twoPi * 2.0,
                                   juce::MathConstants<double>::twoPi);
            if (reelAngle < 0.0)
                reelAngle += juce::MathConstants<double>::twoPi;
        }

        reelLastPos = pos;
    }
    else
    {
        reelLastPos = -1.0f;
    }

    const bool changed = pulseActive
                      || std::abs (pos - lastPos) > 0.00002f
                      || cycles != lastCycles
                      || rec != lastRecording
                      || play != lastPlaying
                      || std::abs (wear - lastWear) > 0.001f
                      || std::abs (dropout - lastDropout) > 0.005f
                      || dir != lastDirection
                      || std::abs (tape - lastTape) > 0.001f
                      || std::abs (start - lastStart) > 0.0005f
                      || std::abs (end - lastEnd) > 0.0005f
                      || hasTape != lastHasContent
                      || writeVersion != lastWriteVersion
                      || std::abs (envLevel - lastEnvLevel) > 0.004f
                      || envStage != lastEnvStage;

    if (! changed)
        return;

    // Rebuild the waveform when the recorded material actually changes.  The
    // write version is essential in synced mode: the tape length is fixed from
    // the first block, so a growing take never changed it and the map stayed
    // blank until the length was edited.
    if (hasTape && (! waveValid
                    || writeVersion != waveCachedVersion
                    || std::abs (tape - waveCachedTape) > 0.005f))
    {
        proc.copyLoopWaveform (minV.data(), maxV.data(), kBuckets);
        waveCachedTape = tape;
        waveCachedVersion = writeVersion;
        waveValid = true;
        waveDirty = true;        // repaint the cached bitmap in place
    }
    else if (! hasTape)
    {
        waveValid = false;
        waveCachedTape = -1.0f;
        waveCachedVersion = 0;
        waveImage = {};
        waveImageWidth = 0;
        waveScaleSeeded = false;

        // An erased tape has no past, so there is nothing to ghost.
        std::fill (erosionMax.begin(), erosionMax.end(), 0.0f);
        std::fill (erosionMin.begin(), erosionMin.end(), 0.0f);
        lastErosionMs = 0.0;
    }

    lastPos = pos;
    lastCycles = cycles;
    lastRecording = rec;
    lastPlaying = play;
    lastWear = wear;
    lastDropout = dropout;
    lastDirection = dir;
    lastTape = tape;
    lastStart = start;
    lastEnd = end;
    lastHasContent = hasTape;
    lastWriteVersion = writeVersion;
    lastEnvLevel = envLevel;
    lastEnvStage = envStage;

    repaint();
}

//==============================================================================
void TapeDisplay::initWatchedParams()
{
    // Parameters whose effect is visible on the tape map.  The group chooses the
    // colour of the feedback flash: tone (teal), time (lavender), decay (copper).
    watchedParams =
    {
        { ids::wear,      "WEAR",      "level and top end fall away",        2 },
        { ids::dropouts,  "DROPOUTS",  "fragments drop out on the grid",     2 },
        { ids::splice,    "SPLICE",    "the head jumps a phrase",            1 },
        { ids::oxide,     "OXIDE",     "random patches lose the high end",   0 },
        { ids::wow,       "WOW",       "the pitch drifts as the tape drags", 1 },
        { ids::hiss,      "HISS",      "the noise floor rises",              0 },
        { ids::tone,      "TONE",      "the low-pass character of the tape", 0 },
        { ids::sat,       "SAT",       "drive and gentle compression",       2 },
        { ids::speed,     "SPEED",     "varispeed: pitch and time together", 1 },
        { ids::feedback,  "FEEDBACK",  "more of the loop is written back",   2 },
        { ids::attack,    "ATTACK",    "how long a pass fades in",           1 },
        { ids::release,   "RELEASE",   "how long a pass fades out",          1 },
    };

    // Resolve the pointers once.  They stay valid for the lifetime of the
    // APVTS (a state replace swaps values, not the parameter objects).
    for (auto& wp : watchedParams)
    {
        wp.param = proc.apvts.getParameter (wp.id);
        wp.raw   = proc.apvts.getRawParameterValue (wp.id);
    }
}

float TapeDisplay::rawValue (const char* id) const
{
    for (const auto& wp : watchedParams)
        if (wp.raw != nullptr && std::strcmp (wp.id, id) == 0)
            return wp.raw->load();

    if (auto* v = proc.apvts.getRawParameterValue (id))
        return v->load();

    return 0.0f;
}

juce::RangedAudioParameter* TapeDisplay::paramFor (const char* id) const
{
    for (const auto& wp : watchedParams)
        if (wp.param != nullptr && std::strcmp (wp.id, id) == 0)
            return wp.param;

    return proc.apvts.getParameter (id);
}

void TapeDisplay::detectParamChange()
{
    float biggestDelta = 0.0f;
    int   hit = -1;

    for (size_t i = 0; i < watchedParams.size(); ++i)
    {
        auto& wp = watchedParams[i];

        if (wp.param == nullptr || wp.raw == nullptr)
            continue;

        const float v = wp.raw->load();

        if (! wp.initialised)
        {
            wp.last = v;
            wp.initialised = true;
            continue;
        }

        // Compare in normalised (0..1) units so a 0..1 knob and a -24..+24 knob
        // are measured on the same footing.  convertTo0to1 is the parameter's own
        // mapping, so no range lookup is needed here.
        const float delta = std::abs (wp.param->convertTo0to1 (v)
                                      - wp.param->convertTo0to1 (wp.last));

        if (delta > 0.004f && delta > biggestDelta)
        {
            biggestDelta = delta;
            hit = (int) i;
        }

        wp.last = v;
    }

    if (hit < 0)
        return;

    const auto& wp = watchedParams[(size_t) hit];

    paramPulseText = wp.label;
    paramPulseEffect = wp.effect;
    paramPulseId = wp.id;
    paramPulseGroup = wp.group;
    paramPulseStartMs = juce::Time::getMillisecondCounterHiRes();
}

void TapeDisplay::drawParamFeedback (juce::Graphics& g) const
{
    if (paramPulseStartMs < -1.0e8 || waveArea.getWidth() <= 0.0f)
        return;

    const float progress = pulseProgress();

    if (progress >= 1.0f)
        return;

    const auto text = findColour (SomniaLookAndFeel::textColourId);
    const auto tint = pulseTint();

    // Ease the tail so the flash does not stop dead.
    const float fade = 1.0f - progress * progress;

    // --- travelling head of response -----------------------------------------
    // The flash sweeps across the map the way the play head sweeps the tape: it
    // shows *where on the loop* the change is being applied right now.
    const float px = waveArea.getX() + progress * waveArea.getWidth();
    const float halfW = 26.0f;

    for (int i = 0; i < 14; ++i)
    {
        const float t = (float) i / 13.0f;
        const float x = px - halfW + t * halfW * 2.0f;
        const float a = fade * (1.0f - std::abs (t - 1.0f)) * 0.15f;

        if (a <= 0.005f)
            continue;

        g.setColour (tint.withAlpha (a));
        g.drawLine (x, waveArea.getY() + 2.0f, x, waveArea.getBottom() - 2.0f, 1.5f);
    }

    // --- frame of the map lights up in the group colour ----------------------
    g.setColour (tint.withAlpha (0.40f * fade));
    g.drawRoundedRectangle (waveArea.reduced (0.5f), 6.0f, 1.5f);

    // --- what moved and what it does to the wave ------------------------------
    const float labelAlpha = progress < 0.7f ? 1.0f : (1.0f - progress) / 0.3f;
    auto row = juce::Rectangle<float> (280.0f, 30.0f)
                   .withPosition (waveArea.getX() + 10.0f, waveArea.getY() + 8.0f);

    if (row.getRight() > waveArea.getRight() - 6.0f)
        row.setX (waveArea.getRight() - 286.0f);

    g.setColour (findColour (SomniaLookAndFeel::scopeColourId).withAlpha (0.88f * labelAlpha));
    g.fillRoundedRectangle (row, 4.0f);
    g.setColour (tint.withAlpha (0.55f * labelAlpha));
    g.drawRoundedRectangle (row.reduced (0.5f), 4.0f, 1.0f);

    g.setColour (tint.withAlpha (labelAlpha));
    g.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Bold")));
    g.drawText (paramPulseText, row.withTrimmedLeft (9.0f).removeFromTop (15),
                juce::Justification::centredLeft, false);

    g.setColour (text.withAlpha (0.80f * labelAlpha));
    g.setFont (juce::Font (juce::FontOptions (9.5f)));
    g.drawText (paramPulseEffect, row.withTrimmedLeft (9.0f).removeFromBottom (14),
                juce::Justification::centredLeft, false);
}

//==============================================================================
float TapeDisplay::pulseProgress() const
{
    if (paramPulseStartMs < -1.0e8)
        return 1.0f;

    constexpr double kPulseMs = 1250.0;
    const double age = juce::Time::getMillisecondCounterHiRes() - paramPulseStartMs;
    return (float) juce::jlimit (0.0, 1.0, age / kPulseMs);
}

juce::Colour TapeDisplay::pulseTint() const
{
    if (paramPulseGroup == 0)
        return findColour (SomniaLookAndFeel::coolColourId);

    if (paramPulseGroup == 1)
        return findColour (SomniaLookAndFeel::accentColourId);

    return findColour (SomniaLookAndFeel::hotColourId);
}

//==============================================================================
void TapeDisplay::drawWavePrediction (juce::Graphics& g, float progress,
                                      juce::Colour tint) const
{
    if (! proc.getDisplayHasContent() || waveArea.getWidth() < 8.0f)
        return;

    if ((int) predMin.size() != kBuckets || (int) predMax.size() != kBuckets)
        return;

    const float fadeIn  = juce::jlimit (0.0f, 1.0f, progress / 0.10f);
    const float fadeOut = 1.0f - juce::jlimit (0.0f, 1.0f, (progress - 0.55f) / 0.45f);
    const float alpha   = fadeIn * fadeOut;

    if (alpha <= 0.02f)
        return;

    auto param = [this] (const char* id) { return rawValue (id); };

    // Attack and release do not change the shape of the material, they fade the
    // pass in and out; a gradient over the loop window reads that directly.
    if (paramPulseId == ids::attack || paramPulseId == ids::release)
    {
        const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
        const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
        const float tape  = proc.getDisplayTapeLengthSeconds();
        const float loopSec = juce::jmax (0.05f, (end - start) * tape);
        const float t = juce::jlimit (0.02f, 1.0f,
                                      param (paramPulseId == ids::attack ? ids::attack
                                                                         : ids::release)
                                          * 0.001f / loopSec);

        const float x0 = xAt (start);
        const float x1 = xAt (end);
        const float w  = (x1 - x0) * t;
        const bool  isAttack = paramPulseId == ids::attack;

        auto region = juce::Rectangle<float> (isAttack ? x0 : x1 - w,
                                              waveArea.getY(), w, waveArea.getHeight());

        juce::ColourGradient grad (tint.withAlpha (isAttack ? 0.0f : 0.40f * alpha),
                                   region.getX(), 0.0f,
                                   tint.withAlpha (isAttack ? 0.40f * alpha : 0.0f),
                                   region.getRight(), 0.0f, false);
        g.setGradientFill (grad);
        g.fillRect (region);
        return;
    }

    // Feedback does not reshape one pass either: it repeats whole passes, so the
    // travelling sweep and the label are the honest feedback for it.
    if (paramPulseId == ids::feedback)
        return;

    // The prediction starts as a copy of the recorded buckets and is then put
    // through the same kind of change the parameter applies to the live signal.
    for (int i = 0; i < kBuckets; ++i)
    {
        predMin[(size_t) i] = minV[(size_t) i];
        predMax[(size_t) i] = maxV[(size_t) i];
    }

    auto sampleAt = [] (const std::vector<float>& v, float n)
    {
        const float x = juce::jlimit (0.0f, 1.0f, n) * (float) (kBuckets - 1);
        const int i0 = (int) x;
        const int i1 = juce::jmin (kBuckets - 1, i0 + 1);
        const float t = x - (float) i0;
        return v[(size_t) i0] * (1.0f - t) + v[(size_t) i1] * t;
    };

    const auto& id = paramPulseId;
    const int half = kBuckets / 2;

    if (id == ids::speed)
    {
        // Varispeed: +12 st reads the pass twice as fast (the picture squeezes
        // to half width), -12 st stretches it to double.
        const float k = std::pow (2.0f, -param (ids::speed) / 12.0f);

        for (int i = 0; i < kBuckets; ++i)
        {
            const float n = ((float) i + 0.5f) / (float) kBuckets;
            const float src = 0.5f + (n - 0.5f) * k;

            if (src < 0.0f || src >= 1.0f)
            {
                predMin[(size_t) i] = 0.0f;
                predMax[(size_t) i] = 0.0f;
            }
            else
            {
                predMin[(size_t) i] = sampleAt (minV, src);
                predMax[(size_t) i] = sampleAt (maxV, src);
            }
        }
    }
    else if (id == ids::wow)
    {
        // Wow: the read point drifts sideways, so the picture wobbles.
        const float amount = param (ids::wow);
        const float phase = (float) (juce::Time::getMillisecondCounterHiRes() * 0.002);

        for (int i = 0; i < kBuckets; ++i)
        {
            const float n = ((float) i + 0.5f) / (float) kBuckets;
            const float shift = std::sin (n * juce::MathConstants<float>::twoPi * 2.0f + phase)
                                * amount * 0.03f;
            predMin[(size_t) i] = sampleAt (minV, n + shift);
            predMax[(size_t) i] = sampleAt (maxV, n + shift);
        }
    }
    else if (id == ids::splice)
    {
        // Splice: the second half of the pass is read from another phrase.
        const float amount = param (ids::splice);

        for (int i = 0; i < kBuckets; ++i)
        {
            const float n = ((float) i + 0.5f) / (float) kBuckets;
            const float src = i < half ? n : std::fmod (n + 0.25f * amount, 1.0f);
            predMin[(size_t) i] = sampleAt (minV, src);
            predMax[(size_t) i] = sampleAt (maxV, src);
        }
    }
    else if (id == ids::tone || id == ids::wear || id == ids::oxide)
    {
        // Loss of top end: the envelope loses its fine detail, drawn as a moving
        // average.  Oxide only does it in patches, the way real oxide sheds.
        const float amount = param (id == ids::tone ? ids::tone
                                                    : id == ids::wear ? ids::wear
                                                                      : ids::oxide);
        int radius = 0;
        float gain = 1.0f;

        if (id == ids::tone)
        {
            radius = 1 + (int) ((1.0f - amount) * 9.0f);
            gain = 0.60f + 0.40f * amount;
        }
        else if (id == ids::wear)
        {
            radius = 1 + (int) (amount * 4.0f);
            gain = 1.0f - 0.45f * amount;
        }
        else
        {
            radius = 1 + (int) (amount * 7.0f);
        }

        for (int i = 0; i < kBuckets; ++i)
        {
            if (id == ids::oxide)
            {
                const std::uint32_t h = (std::uint32_t) (i / 24) * 2654435761u;
                if ((h >> 28) >= (std::uint32_t) (amount * 14.0f))
                    continue;
            }

            float lo = 0.0f, hi = 0.0f;
            int count = 0;

            for (int j = -radius; j <= radius; ++j)
            {
                const int k = juce::jlimit (0, kBuckets - 1, i + j);
                lo += minV[(size_t) k];
                hi += maxV[(size_t) k];
                ++count;
            }

            predMin[(size_t) i] = lo / (float) count * gain;
            predMax[(size_t) i] = hi / (float) count * gain;
        }
    }
    else if (id == ids::dropouts)
    {
        // Dropouts: whole fragments of the pass are simply missing.
        const float amount = param (ids::dropouts);
        const int run = 7;

        for (int i = 0; i < kBuckets; ++i)
        {
            const std::uint32_t h = (std::uint32_t) (i / run) * 2246822519u;
            if ((h >> 29) < (std::uint32_t) (amount * 6.0f))
            {
                predMin[(size_t) i] = 0.0f;
                predMax[(size_t) i] = 0.0f;
            }
        }
    }
    else if (id == ids::hiss)
    {
        // Hiss: a noise floor that does not follow the signal.  The floor is
        // sized in pixels and converted back into signal units, because the
        // waveform is normalised to its own peak: on a quiet take waveScale is
        // large, and a floor expressed in signal units blew up past the panel.
        const float floorPx = param (ids::hiss) * waveArea.getHeight() * 0.14f;
        const float floor = waveScale > 1.0e-6f ? floorPx / waveScale : 0.0f;
        std::uint32_t seed = 0x9e3779b9u;

        for (int i = 0; i < kBuckets; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float r = (float) ((seed >> 8) & 0xffffu) / 65535.0f - 0.5f;
            predMin[(size_t) i] = juce::jmin (predMin[(size_t) i], -floor + r * floor * 0.5f);
            predMax[(size_t) i] = juce::jmax (predMax[(size_t) i],  floor + r * floor * 0.5f);
        }
    }
    else if (id == ids::sat)
    {
        // Saturation: peaks round off and quiet detail lifts.
        const float drive = 1.0f + param (ids::sat) * 3.0f;
        const float norm = std::tanh (drive);

        for (int i = 0; i < kBuckets; ++i)
        {
            predMin[(size_t) i] = std::tanh (predMin[(size_t) i] * drive) / norm;
            predMax[(size_t) i] = std::tanh (predMax[(size_t) i] * drive) / norm;
        }
    }

    // --- draw the predicted shape --------------------------------------------
    // The prediction is clipped to the rounded screen: without the clip a wide
    // shape (a raised noise floor, a stretched wave) painted over the corners
    // and read as the wave escaping the panel.
    const float cy = waveArea.getCentreY();
    const float w  = waveArea.getWidth();

    juce::Graphics::ScopedSaveState clip (g);
    {
        juce::Path screen;
        screen.addRoundedRectangle (waveArea, 6.0f);
        g.reduceClipRegion (screen);
    }

    juce::Path band;

    for (int i = 0; i < kBuckets; ++i)
    {
        const float x = waveArea.getX() + (((float) i + 0.5f) / (float) kBuckets) * w;
        const float y = juce::jlimit (waveArea.getY(), waveArea.getBottom(),
                                      cy - predMax[(size_t) i] * waveScale);

        if (i == 0)
            band.startNewSubPath (x, y);
        else
            band.lineTo (x, y);
    }

    for (int i = kBuckets - 1; i >= 0; --i)
    {
        const float x = waveArea.getX() + (((float) i + 0.5f) / (float) kBuckets) * w;
        const float y = juce::jlimit (waveArea.getY(), waveArea.getBottom(),
                                      cy - predMin[(size_t) i] * waveScale);
        band.lineTo (x, y);
    }

    band.closeSubPath();

    // Half the weight of the first pass: the prediction is a hint about what the
    // knob is doing, not a second waveform competing with the recorded one.
    g.setColour (tint.withAlpha (0.11f * alpha));
    g.fillPath (band);
    g.setColour (tint.withAlpha (0.45f * alpha));
    g.strokePath (band, juce::PathStrokeType (1.2f));
}

//==============================================================================
float TapeDisplay::normAt (float x) const
{
    if (waveArea.getWidth() <= 0.0f)
        return 0.0f;

    return juce::jlimit (0.0f, 1.0f, (x - waveArea.getX()) / waveArea.getWidth());
}

float TapeDisplay::xAt (float norm) const
{
    return waveArea.getX() + juce::jlimit (0.0f, 1.0f, norm) * waveArea.getWidth();
}

float TapeDisplay::minNorm() const
{
    const float tape = proc.getDisplayTapeLengthSeconds();
    if (tape <= 0.0f)
        return 0.05f;

    // 50 ms expressed as a fraction of the tape.  The lower bound only stops a
    // degenerate value; it used to be 0.002, which silently turned the 50 ms
    // minimum into 120 ms on a 60 s tape.
    return juce::jlimit (0.0005f, 0.5f, 0.05f / tape);
}

int TapeDisplay::markerAt (float x) const
{
    if (waveArea.isEmpty() || ! proc.getDisplayHasContent())
        return -1;

    const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
    const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
    const float sx = xAt (start);
    const float ex = xAt (end);

    // Nearest marker wins.  Checking START first made the END handle unreachable
    // whenever the loop was shorter than the 2 * tolerance window: the two
    // handles overlap and START always answered.
    const float tolerance = 10.0f;
    const float ds = std::abs (x - sx);
    const float de = std::abs (x - ex);

    if (ds <= tolerance || de <= tolerance)
        return ds <= de ? 0 : 1;

    const float n = normAt (x);
    if (n > start && n < end)
        return 2;

    return -1;
}

void TapeDisplay::setBound (int which, float value)
{
    auto* p = paramFor (which == 0 ? ids::loopStart : ids::loopEnd);
    if (p == nullptr)
        return;

    p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (0.0f, 1.0f, value)));
}

//==============================================================================
void TapeDisplay::drawReel (juce::Graphics& g, juce::Point<float> centre,
                            float radius, float angle, bool active, float fill) const
{
    const auto panel = findColour (SomniaLookAndFeel::panelColourId);
    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);

    // Recessed well.  Derived from the panel so it reads as a shadowed recess in
    // both themes (a fixed black wash turned into a heavy dark disc in light).
    g.setColour (panel.darker (0.45f));
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    // Wound tape: the pack shrinks and grows with the play position.
    const float tapeD = radius * 2.0f * (0.58f + 0.32f * juce::jlimit (0.0f, 1.0f, fill));
    g.setColour (accentDim.withAlpha (active ? 0.78f : 0.38f));
    g.fillEllipse (juce::Rectangle<float> (tapeD, tapeD).withCentre (centre));

    // Hub plate.
    g.setColour (panel.darker (0.35f));
    g.fillEllipse (juce::Rectangle<float> (radius * 1.12f, radius * 1.12f).withCentre (centre));

    // Hub.
    g.setColour (panel.brighter (0.18f));
    g.fillEllipse (juce::Rectangle<float> (radius * 0.52f, radius * 0.52f).withCentre (centre));

    g.setColour (accentDim.withAlpha (0.85f));
    g.drawEllipse (juce::Rectangle<float> (radius * 0.52f, radius * 0.52f).withCentre (centre),
                   1.2f);

    // Spokes.
    g.setColour (accent.withAlpha (active ? 0.85f : 0.35f));
    for (int i = 0; i < 6; ++i)
    {
        const float a = angle + juce::MathConstants<float>::twoPi * (float) i / 6.0f;
        const float inner = radius * 0.17f;
        const float outer = radius * 0.49f;
        g.drawLine (centre.x + std::cos (a) * inner, centre.y + std::sin (a) * inner,
                    centre.x + std::cos (a) * outer, centre.y + std::sin (a) * outer,
                    2.0f);
    }

    g.setColour (juce::Colours::black.withAlpha (0.30f));
    g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre),
                   1.5f);
}

//==============================================================================
void TapeDisplay::drawEnvelope (juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);
    const auto hot = findColour (SomniaLookAndFeel::hotColourId);
    const auto textDim = findColour (SomniaLookAndFeel::textDimColourId);

    // Without any material the wet path is silent, so an envelope that the DSP
    // keeps running would be a false indicator: show it as idle.  A fresh take is
    // the same case — the read head is idle and nothing is audible yet.
    const bool hasTape = proc.getDisplayHasContent()
                      && ! proc.getDisplayFreshRecording();
    const float level = hasTape ? juce::jlimit (0.0f, 1.0f, proc.getDisplayEnvelopeLevel())
                                : 0.0f;
    const int   stage = hasTape ? proc.getDisplayEnvelopeStage() : 0;
    const float sustain = juce::jlimit (0.0f, 1.0f,
                                        (sustainValue != nullptr ? sustainValue->load() : 0.0f));

    // The envelope is part of the header strip, not a separate window: there is
    // no panel or frame around it, only the shape and a marker that follows the
    // stage.  A label sits right next to the curve.
    auto plot = area.removeFromLeft (92.0f).reduced (0.0f, 4.0f);

    const float w = plot.getWidth();
    const float h = plot.getHeight();
    auto yOf = [&] (float v) { return plot.getBottom() - v * h; };

    const float ax = plot.getX() + w * 0.22f;   // end of attack
    const float dx = plot.getX() + w * 0.44f;   // end of decay
    const float sx = plot.getX() + w * 0.72f;   // end of sustain
    const float rx = plot.getX() + w * 0.96f;   // end of release

    juce::Path shape;
    shape.startNewSubPath (plot.getX(), plot.getBottom());
    shape.lineTo (ax, yOf (1.0f));
    shape.lineTo (dx, yOf (sustain));
    shape.lineTo (sx, yOf (sustain));
    shape.lineTo (rx, plot.getBottom());

    // A very light fill under the curve keeps it readable on the dark header
    // without turning it into a boxed widget.
    juce::Path filled (shape);
    filled.lineTo (rx, plot.getBottom());
    filled.closeSubPath();
    g.setColour (accent.withAlpha (0.09f));
    g.fillPath (filled);

    g.setColour (accentDim.withAlpha (0.70f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));

    // Marker: x follows the current stage, y is the real envelope level.
    float mx = plot.getX();

    if (stage == 1)
        mx = plot.getX() + (ax - plot.getX()) * level;
    else if (stage == 2)
        mx = ax + (dx - ax) * juce::jlimit (0.0f, 1.0f,
                                            (1.0f - level) / juce::jmax (0.001f, 1.0f - sustain));
    else if (stage == 3)
        mx = dx + (sx - dx) * 0.5f;
    else if (stage == 4)
        mx = sx + (rx - sx) * juce::jlimit (0.0f, 1.0f,
                                            sustain > 0.001f ? 1.0f - level / sustain : 1.0f);

    const float my = yOf (level);
    g.setColour ((stage == 0 ? accentDim : hot).withAlpha (stage == 0 ? 0.45f : 0.95f));
    g.fillEllipse (juce::Rectangle<float> (4.5f, 4.5f).withCentre ({ mx, my }));

    // Stage label.
    static const char* const names[] = { "IDLE", "ATT", "DEC", "SUS", "REL" };
    const int idx = juce::jlimit (0, 4, stage);

    g.setColour (stage == 0 ? textDim.withAlpha (0.75f) : accent);
    g.setFont (juce::Font (juce::FontOptions (8.5f).withStyle ("Bold")));
    g.drawText (names[idx], area.withTrimmedLeft (6.0f),
                juce::Justification::centredLeft, false);
}

//==============================================================================
void TapeDisplay::drawHeader (juce::Graphics& g) const
{
    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);
    const auto hot = findColour (SomniaLookAndFeel::hotColourId);
    const auto textDim = findColour (SomniaLookAndFeel::textDimColourId);

    auto h = headerArea;

    // --- transport lamps -----------------------------------------------------
    {
        auto lamps = h.removeFromLeft (140.0f);
        const float r = 4.0f;

        auto lamp = [&] (juce::Rectangle<float> row, bool on, juce::Colour colour,
                         const juce::String& label)
        {
            const auto c = juce::Point<float> (row.getX() + r + 1.0f, row.getCentreY());
            g.setColour (on ? colour : colour.withAlpha (0.16f));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));

            g.setColour (on ? colour : textDim.withAlpha (0.65f));
            g.setFont (juce::Font (juce::FontOptions (9.5f).withStyle ("Bold")));
            g.drawText (label, row.withTrimmedLeft (r * 2.0f + 5.0f),
                        juce::Justification::centredLeft, false);
        };

        lamp (lamps.removeFromLeft (66.0f), proc.getDisplayRecording(), hot, "REC");
        lamp (lamps, proc.getDisplayPlaying(), accent, "PLAY");
    }

    // --- envelope indicator --------------------------------------------------
    {
        auto env = h.removeFromLeft (134.0f);
        drawEnvelope (g, env);
    }

    // --- wear meter ----------------------------------------------------------
    {
        auto wearRow = h.removeFromRight (150.0f);
        const float barW = 84.0f;
        auto bar = juce::Rectangle<float> (barW, 5.0f)
                       .withCentre ({ wearRow.getRight() - barW * 0.5f, wearRow.getCentreY() });

        g.setColour (accentDim.withAlpha (0.32f));
        g.fillRoundedRectangle (bar, 2.5f);
        g.setColour (accent.withAlpha (0.9f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth()
                                                   * juce::jlimit (0.0f, 1.0f,
                                                                   proc.getDisplayWear())),
                                2.5f);

        g.setColour (textDim);
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText ("WEAR", wearRow.withTrimmedRight (barW + 8.0f),
                    juce::Justification::centredRight, false);
    }

    // --- pass counter / tempo / tape length ----------------------------------
    g.setColour (textDim);
    g.setFont (juce::Font (juce::FontOptions (9.5f)));
    g.drawText ("PASS " + juce::String (proc.getDisplayCycles()).paddedLeft ('0', 2)
                    + "   " + juce::String (proc.getDisplayBpm(), 0) + " BPM"
                    + (proc.getDisplayHasContent()
                           ? "   TAPE " + juce::String (proc.getDisplayTapeLengthSeconds(), 2) + " s"
                           : juce::String ("   EMPTY TAPE")),
                h, juce::Justification::centred, false);
}

//==============================================================================
void TapeDisplay::rebuildWaveImage() const
{
    const int w = (int) waveArea.getWidth();
    const int h = (int) waveArea.getHeight();

    if (w <= 0 || h <= 0)
        return;

    // Reuse the bitmap: it is rebuilt on every take update, and allocating a
    // fresh ARGB image of the panel each time churned ~800 kB per rebuild in the
    // UI thread.  A resize still needs a new allocation.
    if (waveImage.isNull()
        || waveImage.getWidth() != w || waveImage.getHeight() != h)
    {
        waveImage = juce::Image (juce::Image::ARGB, w, h, true);
    }
    else
    {
        waveImage.clear (waveImage.getBounds());
    }

    waveImageWidth = w;
    waveImageHeight = h;
    waveDirty = false;

    juce::Graphics ig (waveImage);

    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);

    // The bitmap is transparent: the scope fill (and its rounded corners) is
    // drawn live underneath it in drawWave, so the theme can change without
    // baking a square background into the cache.

    const float cy  = (float) h * 0.5f;
    const float amp = (float) h * 0.46f;

    // Normalise the take to its own peak.  Without this a typical recorded loop
    // (peak around 0.3..0.5) only filled a fifth of the panel and read as a
    // hairline.  The gain ceiling keeps a tape that holds nothing but hiss from
    // being blown up to full height.
    float peak = 0.0f;
    for (int i = 0; i < kBuckets; ++i)
        peak = juce::jmax (peak, juce::jmax (std::abs (maxV[(size_t) i]),
                                             std::abs (minV[(size_t) i])));

    const float floorPeak = 0.05f;
    const float targetScale = juce::jmin (amp / juce::jmax (floorPeak, peak),
                                          amp / floorPeak);

    // The scale is eased over time: while a loop is being built the peak climbs
    // take by take, and snapping the normalisation to it would make the whole
    // waveform breathe instead of growing.  The very first take is seeded
    // directly, otherwise the wave visibly swelled from 1.0 over ~20 frames.
    if (! waveScaleSeeded)
    {
        waveScale = targetScale;
        waveScaleSeeded = true;
    }
    else
    {
        waveScale += (targetScale - waveScale) * 0.12f;
    }

    // --- erosion contour (what the tape used to hold) ------------------------
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = lastErosionMs > 0.0 ? (now - lastErosionMs) * 0.001 : 0.0;
    lastErosionMs = now;

    // Half-life of the peak hold: long enough that a pass or two of loss is
    // clearly visible, short enough that the contour settles after a re-record.
    const float hold = (float) std::pow (0.5, juce::jlimit (0.0, 1.0, dt / 2.5));

    for (int i = 0; i < kBuckets; ++i)
    {
        const float mx = maxV[(size_t) i] * waveScale;
        const float mn = minV[(size_t) i] * waveScale;

        erosionMax[(size_t) i] = juce::jmax (erosionMax[(size_t) i] * hold, mx);
        erosionMin[(size_t) i] = juce::jmin (erosionMin[(size_t) i] * hold, mn);
    }

    // --- wave bars -----------------------------------------------------------
    // The panel is ~880 px wide for 512 buckets, so a 1 px line leaves gaps.  A
    // bar of ceil(w / kBuckets) px makes the wave a solid, readable shape.
    const int barW = juce::jmax (1, (int) std::ceil ((float) w / (float) kBuckets));

    // Set once: setColour() per bar meant 1024 state changes per rebuild.
    ig.setColour (accent.withAlpha (0.88f));

    for (int i = 0; i < kBuckets; ++i)
    {
        const float n = ((float) i + 0.5f) / (float) kBuckets;
        const int x = (int) (n * (float) w);

        const float y1 = juce::jlimit (0.0f, (float) h, cy - maxV[(size_t) i] * waveScale);
        const float y2 = juce::jlimit (0.0f, (float) h, cy - minV[(size_t) i] * waveScale);

        const int top = juce::roundToInt (juce::jmin (y1, y2));
        const int bot = juce::roundToInt (juce::jmax (y1, y2));

        ig.fillRect (juce::Rectangle<int> (x, top, barW, juce::jmax (1, bot - top)));
    }

    // --- erosion outline ------------------------------------------------------
    // Only drawn where the contour is meaningfully above the live wave, so the
    // ghost does not just retrace the shape that is already there.
    juce::Path upper, lower;
    bool drewUpper = false, drewLower = false;

    for (int i = 0; i < kBuckets; ++i)
    {
        const float n = ((float) i + 0.5f) / (float) kBuckets;
        const float x = n * (float) w;
        const float liveMax = maxV[(size_t) i] * waveScale;
        const float liveMin = minV[(size_t) i] * waveScale;

        const float eh = erosionMax[(size_t) i];
        const float el = erosionMin[(size_t) i];

        if (eh - liveMax > 1.5f)
        {
            const float y = juce::jlimit (0.0f, (float) h, cy - eh);
            if (drewUpper) upper.lineTo (x, y); else { upper.startNewSubPath (x, y); drewUpper = true; }
        }
        else
        {
            drewUpper = false;
        }

        if (liveMin - el > 1.5f)
        {
            const float y = juce::jlimit (0.0f, (float) h, cy - el);
            if (drewLower) lower.lineTo (x, y); else { lower.startNewSubPath (x, y); drewLower = true; }
        }
        else
        {
            drewLower = false;
        }
    }

    ig.setColour (accentDim.withAlpha (0.75f));
    ig.strokePath (upper, juce::PathStrokeType (1.1f));
    ig.strokePath (lower, juce::PathStrokeType (1.1f));
}

//==============================================================================
void TapeDisplay::drawWave (juce::Graphics& g) const
{
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);
    const auto hot       = findColour (SomniaLookAndFeel::hotColourId);
    const auto cool      = findColour (SomniaLookAndFeel::coolColourId);
    const auto textDim   = findColour (SomniaLookAndFeel::textDimColourId);
    const auto scope     = findColour (SomniaLookAndFeel::scopeColourId);
    const auto scopeDim  = findColour (SomniaLookAndFeel::scopeDimColourId);
    const auto ink       = findColour (SomniaLookAndFeel::textColourId);

    g.setColour (scope);
    g.fillRoundedRectangle (waveArea, 6.0f);
    g.setColour (accentDim.withAlpha (0.35f));
    g.drawRoundedRectangle (waveArea.reduced (0.5f), 6.0f, 1.0f);

    const float tape = proc.getDisplayTapeLengthSeconds();
    const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
    const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
    const bool  hasTape = proc.getDisplayHasContent();

    if (! hasTape)
    {
        g.setColour (textDim.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("RECORD TO CREATE A LOOP", waveArea,
                    juce::Justification::centred, false);
    }
    else
    {
        const float cy  = waveArea.getCentreY();

        // --- waveform (cached image) -----------------------------------------
        // 512 vertical lines per frame is what made the panel feel sluggish.
        // The bars are rendered once into an image and blitted; the dimming,
        // markers and playhead are drawn live on top.
        if (waveImage.isNull() || waveDirty
            || waveImageWidth  != (int) waveArea.getWidth()
            || waveImageHeight != (int) waveArea.getHeight())
        {
            rebuildWaveImage();
        }

        if (! waveImage.isNull())
            g.drawImageAt (waveImage, (int) waveArea.getX(), (int) waveArea.getY(), false);

        // The tape path: a faint line that visually connects both reels.
        g.setColour (accentDim.withAlpha (0.18f));
        g.drawHorizontalLine (juce::roundToInt (cy),
                              waveArea.getX() + 2.0f, waveArea.getRight() - 2.0f);

        // The wave as the knob leaves it, drawn before the markers and the play
        // head so the loop window stays on top of it.
        drawWavePrediction (g, pulseProgress(), pulseTint());

        // --- dim everything outside the loop window --------------------------
        // Clipped to the rounded scope: plain rectangles painted over the
        // corners and read as the panel being cut off.
        const float sx = xAt (start);
        const float ex = xAt (end);

        {
            juce::Graphics::ScopedSaveState clip (g);
            juce::Path screen;
            screen.addRoundedRectangle (waveArea, 6.0f);
            g.reduceClipRegion (screen);

            g.setColour (scopeDim);
            if (sx > waveArea.getX())
                g.fillRect (juce::Rectangle<float> (waveArea.getX(), waveArea.getY(),
                                                    sx - waveArea.getX(), waveArea.getHeight()));
            if (ex < waveArea.getRight())
                g.fillRect (juce::Rectangle<float> (ex, waveArea.getY(),
                                                    waveArea.getRight() - ex, waveArea.getHeight()));
        }

        // --- selection edges and handles -------------------------------------
        g.setColour (hot.withAlpha (0.95f));
        g.drawVerticalLine (juce::roundToInt (sx), waveArea.getY(), waveArea.getBottom());
        g.fillRect (juce::Rectangle<float> (sx - 2.0f, waveArea.getY(), 4.0f, 10.0f));
        g.fillRect (juce::Rectangle<float> (sx - 2.0f, waveArea.getBottom() - 10.0f, 4.0f, 10.0f));

        g.setColour (cool.withAlpha (0.95f));
        g.drawVerticalLine (juce::roundToInt (ex), waveArea.getY(), waveArea.getBottom());
        g.fillRect (juce::Rectangle<float> (ex - 2.0f, waveArea.getY(), 4.0f, 10.0f));
        g.fillRect (juce::Rectangle<float> (ex - 2.0f, waveArea.getBottom() - 10.0f, 4.0f, 10.0f));

        // --- playhead --------------------------------------------------------
        // During a fresh take nothing is read back yet: the read head is parked
        // at the start, so drawing it would claim the tape is playing.
        if (! proc.getDisplayFreshRecording()
            && (proc.getDisplayPlaying() || proc.getDisplayRecording()))
        {
            const float pos = proc.getDisplayLoopPosition();
            const float px = xAt (start + pos * (end - start));

            g.setColour (ink.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (px - 3.0f, waveArea.getY(),
                                                6.0f, waveArea.getHeight()));
            g.setColour (ink.withAlpha (0.92f));
            g.drawVerticalLine (juce::roundToInt (px),
                                waveArea.getY() + 8.0f, waveArea.getBottom() - 8.0f);
        }

        // --- direction arrow -------------------------------------------------
        {
            const float ax = waveArea.getRight() - 16.0f;
            const float ay = waveArea.getY() + 12.0f;
            juce::Path arrow;

            if (proc.getDirectionSign() >= 0)
            {
                arrow.startNewSubPath (ax - 6.0f, ay);
                arrow.lineTo (ax + 4.0f, ay);
                arrow.startNewSubPath (ax + 4.0f, ay);
                arrow.lineTo (ax, ay - 4.0f);
                arrow.startNewSubPath (ax + 4.0f, ay);
                arrow.lineTo (ax, ay + 4.0f);
            }
            else
            {
                arrow.startNewSubPath (ax + 6.0f, ay);
                arrow.lineTo (ax - 4.0f, ay);
                arrow.startNewSubPath (ax - 4.0f, ay);
                arrow.lineTo (ax, ay - 4.0f);
                arrow.startNewSubPath (ax - 4.0f, ay);
                arrow.lineTo (ax, ay + 4.0f);
            }

            g.setColour (accentDim.withAlpha (0.85f));
            g.strokePath (arrow, juce::PathStrokeType (1.5f));
        }
    }

    // Knob feedback sits on top of the map, drawn even for an empty tape so a
    // parameter move is always acknowledged.
    drawParamFeedback (g);

    // --- reels ---------------------------------------------------------------
    const float reelR = juce::jmin (leftReelArea.getWidth(), leftReelArea.getHeight()) * 0.42f;

    // reelAngle is integrated from the real head movement in timerCallback, so
    // the hubs turn exactly the way the tape runs: forward, reverse, ping-pong.
    const float angle = (float) reelAngle;

    const float baseFill = hasTape
                               ? juce::jlimit (0.18f, 1.0f, tape / 60.0f)
                               : 0.0f;
    const float pos = proc.getDisplayLoopPosition();
    const float leftFill  = baseFill * (1.0f - 0.42f * pos);
    const float rightFill = baseFill * (0.58f + 0.42f * pos);

    drawReel (g, leftReelArea.getCentre(), reelR, angle,
              proc.getDisplayPlaying(), leftFill);
    drawReel (g, rightReelArea.getCentre(), reelR, angle * 0.94f,
              proc.getDisplayPlaying(), rightFill);
}

//==============================================================================
void TapeDisplay::drawFooter (juce::Graphics& g) const
{
    const auto accent = findColour (SomniaLookAndFeel::accentColourId);
    const auto textDim = findColour (SomniaLookAndFeel::textDimColourId);

    const float tape = proc.getDisplayTapeLengthSeconds();
    const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
    const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);

    if (! proc.getDisplayHasContent())
    {
        g.setColour (textDim.withAlpha (0.7f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText ("drag the markers to choose the loop", footerArea,
                    juce::Justification::centredLeft, false);
        return;
    }

    g.setColour (textDim);
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    g.drawText ("START " + juce::String (start * tape, 2) + " s"
                + "   END " + juce::String (end * tape, 2) + " s",
                footerArea, juce::Justification::centredLeft, false);

    g.setColour (accent.withAlpha (0.85f));
    g.drawText ("LOOP " + juce::String ((end - start) * tape, 2) + " s",
                footerArea, juce::Justification::centredRight, false);
}

//==============================================================================
void TapeDisplay::paint (juce::Graphics& g)
{
    const auto panel = findColour (SomniaLookAndFeel::panelColourId);
    const auto accentDim = findColour (SomniaLookAndFeel::accentDimColourId);

    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (accentDim.withAlpha (0.45f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    drawHeader (g);
    drawWave (g);
    drawFooter (g);
}

//==============================================================================
void TapeDisplay::mouseDown (const juce::MouseEvent& e)
{
    // Only the map is draggable.  markerAt() clamps the position into 0..1, so a
    // click on a reel (or in the header) used to land exactly on the START
    // marker of a full-window loop and start dragging it.
    if (! waveArea.contains (e.position))
    {
        dragMarker = -1;
        dragTargetValid = false;
        return;
    }

    dragMarker = markerAt (e.position.x);

    if (dragMarker == 2)
    {
        const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
        const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
        dragOffset = normAt (e.position.x) - start;
        dragLength = end - start;
    }

    dragTargetValid = false;
    lastDragPushMs = 0.0;
}

void TapeDisplay::applyDragTarget()
{
    if (! dragTargetValid || dragMarker < 0)
        return;

    const float n = dragTargetNorm;
    const float start = (loopStartValue != nullptr ? loopStartValue->load() : 0.0f);
    const float end   = (loopEndValue != nullptr ? loopEndValue->load() : 1.0f);
    const float minLen = minNorm();

    if (dragMarker == 0)
    {
        setBound (0, juce::jlimit (0.0f, juce::jmax (0.0f, end - minLen), n));
    }
    else if (dragMarker == 1)
    {
        setBound (1, juce::jlimit (juce::jmin (1.0f, start + minLen), 1.0f, n));
    }
    else if (dragMarker == 2)
    {
        const float newStart = juce::jlimit (0.0f, juce::jmax (0.0f, 1.0f - dragLength),
                                             n - dragOffset);
        const float newEnd = newStart + dragLength;

        // Update in the direction of the move so the pair never inverts in the
        // middle of a block, which would clamp the window and restart the fade.
        if (newStart < start)
        {
            setBound (0, newStart);
            setBound (1, newEnd);
        }
        else
        {
            setBound (1, newEnd);
            setBound (0, newStart);
        }
    }
}

void TapeDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMarker < 0)
        return;

    dragTargetNorm = normAt (e.position.x);
    dragTargetValid = true;

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastDragPushMs < 16.0)
        return;

    lastDragPushMs = now;
    applyDragTarget();
}

void TapeDisplay::mouseUp (const juce::MouseEvent&)
{
    // Commit the exact position the pointer was released at.
    applyDragTarget();
    dragTargetValid = false;
    dragMarker = -1;
}

void TapeDisplay::mouseMove (const juce::MouseEvent& e)
{
    if (! waveArea.contains (e.position))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    const int marker = markerAt (e.position.x);

    if (marker == 0 || marker == 1)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (marker == 2)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TapeDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Resetting the whole window from a double-click on a reel was surprising;
    // the reset belongs to the map.
    if (! waveArea.contains (e.position))
        return;

    setBound (0, 0.0f);
    setBound (1, 1.0f);
    dragTargetValid = false;
    dragMarker = -1;
}

} // namespace somnia
