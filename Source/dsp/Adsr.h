#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace somnia
{

//==============================================================================
//  A small, linear ADSR gate for the loop output.
//
//  Somnia retriggers the envelope on every pass of the tape (and on PLAY/REC),
//  so a long loop breathes once per revolution while a short loop pulses.  The
//  envelope is applied only to the wet signal, never written back into the
//  tape, so it does not accumulate.
//==============================================================================
class Adsr
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        setParams (0.020f, 0.200f, 0.90f, 0.150f);
        reset();
    }

    void reset()
    {
        stage = Stage::idle;
        level = 0.0f;
        releaseStep = 0.0f;
    }

    void setParams (float attackSeconds, float decaySeconds,
                    float sustainLevel, float releaseSeconds)
    {
        attack  = juce::jmax (0.001f, attackSeconds);
        decay   = juce::jmax (0.001f, decaySeconds);
        sustain = juce::jlimit (0.0f, 1.0f, sustainLevel);
        release = juce::jmax (0.001f, releaseSeconds);

        attackStep = 1.0f / (float) juce::jmax (1.0, attack * sampleRate);
        decayStep  = (1.0f - sustain) / (float) juce::jmax (1.0, decay * sampleRate);

        // The release slope is derived from the level at note-off, so a new
        // RELEASE value has to be re-applied to a release that is already
        // running; otherwise the knob does nothing until the next note and the
        // tail keeps the old length.  Only a real change of the parameter
        // re-derives the step: setParams() is called every block, and recomputing
        // unconditionally would turn the linear tail into an asymptote that never
        // reaches zero.
        if (stage == Stage::release && release != lastReleaseUsed)
            restartRelease();
    }

    void noteOn() noexcept
    {
        stage = Stage::attack;
    }

    void noteOff() noexcept
    {
        if (stage == Stage::idle)
            return;

        restartRelease();
        stage = Stage::release;
    }

    float process() noexcept
    {
        switch (stage)
        {
            case Stage::attack:
                level += attackStep;
                if (level >= 1.0f)
                {
                    level = 1.0f;
                    stage = Stage::decay;
                }
                break;

            case Stage::decay:
                level -= decayStep;
                if (level <= sustain)
                {
                    level = sustain;
                    stage = Stage::sustain;
                }
                break;

            case Stage::sustain:
                level = sustain;
                break;

            case Stage::release:
                level -= releaseStep;
                if (level <= 0.0f)
                {
                    level = 0.0f;
                    stage = Stage::idle;
                }
                break;

            case Stage::idle:
            default:
                level = 0.0f;
                break;
        }

        return level;
    }

    bool isActive() const noexcept { return stage != Stage::idle; }
    float getLevel() const noexcept { return level; }

    // 0 = idle, 1 = attack, 2 = decay, 3 = sustain, 4 = release.  Used by the
    // UI to show what the envelope is doing.
    int getStageIndex() const noexcept { return (int) stage; }

private:
    enum class Stage { idle = 0, attack = 1, decay = 2, sustain = 3, release = 4 };

    // Recomputes the release slope from the current level.  Used by noteOff() and
    // by setParams() when the RELEASE knob is turned mid-tail.
    void restartRelease() noexcept
    {
        releaseStep = level / (float) juce::jmax (1.0, release * sampleRate);
        lastReleaseUsed = release;
    }

    double sampleRate = 48000.0;

    float attack = 0.020f, decay = 0.200f, sustain = 0.90f, release = 0.150f;
    float attackStep = 0.0f, decayStep = 0.0f, releaseStep = 0.0f;

    // The release value the running tail was built from; a change of the knob
    // re-derives the slope instead of being ignored until the next note.
    float lastReleaseUsed = 0.150f;

    Stage stage = Stage::idle;
    float level = 0.0f;
};

} // namespace somnia
