#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace somnia
{

//==============================================================================
//  Output stage: a transparent, stereo-linked peak limiter so the wild tape
//  loops never hard-clip the host.  A fast envelope follower drives the gain
//  reduction; a soft (tanh) ceiling catches the tiny overshoot left by the
//  attack ramp.
//==============================================================================
class Limiter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        attackCoeff  = std::exp (-1.0f / (0.001f * (float) sr));   // ~1 ms
        releaseCoeff = std::exp (-1.0f / (0.120f * (float) sr));   // ~120 ms
        reset();
    }

    void reset()
    {
        env = 0.0f;
        gainSmooth = 1.0f;
        lastGrDb = 0.0f;
        holdSamples = 0;
    }

    void process (float* L, float* R, int numSamples, float ceiling)
    {
        const float ceil = juce::jlimit (0.05f, 0.999f, ceiling);
        bool limiting = false;

        for (int n = 0; n < numSamples; ++n)
        {
            const float l = L[n];
            const float r = R[n];

            const float peak = juce::jmax (std::abs (l), std::abs (r));
            if (peak > ceil)
                limiting = true;

            const float coeff = (peak > env) ? attackCoeff : releaseCoeff;
            env = peak + coeff * (env - peak);

            float target = 1.0f;
            if (env > ceil)
                target = ceil / env;

            const float gCoeff = (target < gainSmooth) ? attackCoeff : releaseCoeff;
            gainSmooth = target + gCoeff * (gainSmooth - target);

            L[n] = softCeiling (l * gainSmooth, ceil);
            R[n] = softCeiling (r * gainSmooth, ceil);
        }

        lastGrDb = juce::jmax (0.0f, -juce::Decibels::gainToDecibels (gainSmooth, -60.0f));

        if (limiting)
            holdSamples = (int) (sr * 0.4);
        else
            holdSamples = juce::jmax (0, holdSamples - numSamples);
    }

    float getGainReductionDb() const noexcept { return lastGrDb; }
    bool  isLimiting() const noexcept         { return holdSamples > 0; }

private:
    static float softCeiling (float x, float ceil) noexcept
    {
        const float a = std::abs (x);
        if (a <= ceil)
            return x;

        const float range = juce::jmax (1.0e-4f, 1.0f - ceil);
        const float over  = (a - ceil) / range;
        const float shaped = ceil + range * std::tanh (over);
        return x < 0.0f ? -shaped : shaped;
    }

    double sr = 48000.0;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
    float env = 0.0f;
    float gainSmooth = 1.0f;
    float lastGrDb = 0.0f;
    int   holdSamples = 0;
};

} // namespace somnia
