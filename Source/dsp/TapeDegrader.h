#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace somnia
{

//==============================================================================
//  Tape wear and disintegration.
//
//  This is the heart of Somnia: it makes the loop age.  The same processing is
//  applied both to the signal that leaves the tape and to the signal written
//  back into it (overdub), so every pass loses a little more high end, a few
//  more grains of oxide, and eventually whole fragments drop out — the
//  Basinski "Disintegration Loops" behaviour.
//
//  Stages: wow/flutter (modulated delay) -> tone LP -> oxide LP -> dropouts
//  (with occasional sample-and-hold) -> saturation -> hiss -> wear gain ->
//  DC blocker.
//==============================================================================
class TapeDegrader
{
public:
    struct Params
    {
        float wear     = 0.35f;   // overall generation loss
        float dropouts = 0.25f;   // probability of missing tape fragments
        float oxide    = 0.20f;   // random loss of high frequencies
        float wow      = 0.20f;   // speed instability
        float hiss     = 0.12f;   // tape noise
        float tone     = 0.70f;   // 0 = dark, 1 = open
        float sat      = 0.20f;   // tape saturation

        // Musical quantisation of dropouts: normalised position inside the
        // current phrase (0..1).  Negative disables it.  When set, a dropout is
        // far more likely near a phrase boundary, so gaps land on the grid
        // instead of falling at random.
        float phrasePhase = -1.0f;
    };

    //==========================================================================
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

        delaySize = (int) (0.08 * sampleRate) + 8;
        delayL.assign ((size_t) delaySize, 0.0f);
        delayR.assign ((size_t) delaySize, 0.0f);

        dropoutFallCoeff   = 1.0f - std::exp (-1.0f / (0.002f * (float) sampleRate));
        dropoutRiseCoeff   = 1.0f - std::exp (-1.0f / (0.010f * (float) sampleRate));
        oxideSmoothCoeff   = 1.0f - std::exp (-1.0f / (0.020f * (float) sampleRate));
        envelopeCoeff      = std::exp (-1.0f / (0.250f * (float) sampleRate));

        reset();
    }

    void reset()
    {
        std::fill (delayL.begin(), delayL.end(), 0.0f);
        std::fill (delayR.begin(), delayR.end(), 0.0f);

        delayWrite = 0;
        wowPhase = 0.0;
        flutterPhase = 0.0;
        breathePhase = 0.0;

        lpL = lpR = 0.0f;
        oxideL = oxideR = 0.0f;
        hissLpL = hissLpR = 0.0f;

        dropoutGain = 1.0f;
        dropoutTarget = 1.0f;
        dropoutRemaining = 0;
        dropoutCooldown = 0;
        holding = false;
        holdL = holdR = 0.0f;

        oxideMod = 0.0f;
        oxideTarget = 0.0f;
        oxideTimer = 0;

        envL = envR = 0.0f;
        dcXL = dcXR = dcYL = dcYR = 0.0f;
    }

    //==========================================================================
    // Processes the loop in place.  No allocation, safe on the audio thread.
    void process (float* L, float* R, int numSamples, const Params& p,
                  juce::Random& rng)
    {
        if (L == nullptr || R == nullptr || numSamples <= 0)
            return;

        const float wear     = juce::jlimit (0.0f, 1.0f, p.wear);
        const float dropouts = juce::jlimit (0.0f, 1.0f, p.dropouts);
        const float oxide    = juce::jlimit (0.0f, 1.0f, p.oxide);
        const float wow      = juce::jlimit (0.0f, 1.0f, p.wow);
        const float hiss     = juce::jlimit (0.0f, 1.0f, p.hiss);
        const float tone     = juce::jlimit (0.0f, 1.0f, p.tone);
        const float sat      = juce::jlimit (0.0f, 1.0f, p.sat);

        // ---- tone / wear low-pass ------------------------------------------
        const float baseCutoff = 1200.0f * std::pow (20000.0f / 1200.0f, tone);
        const float wearCutoff = juce::jmax (600.0f, baseCutoff * std::pow (0.18f, wear));
        const float toneCoeff  = std::exp (-2.0f * juce::MathConstants<float>::pi
                                           * wearCutoff / (float) sampleRate);

        // ---- oxide: slow random loss of the top end -------------------------
        // The timer counts samples and process() is called per chunk, so it has
        // to advance by numSamples.  The smoothing coefficient is per-sample, so
        // the block-equivalent value is applied here.
        oxideTimer -= numSamples;
        if (oxideTimer <= 0)
        {
            oxideTarget = rng.nextFloat();
            oxideTimer = (int) ((0.12f + rng.nextFloat() * 0.55f) * (float) sampleRate);
        }
        oxideMod += juce::jmin (1.0f, oxideSmoothCoeff * (float) numSamples)
                    * (oxideTarget - oxideMod);

        const float oxideCutoff = juce::jmax (
            500.0f,
            18000.0f * std::pow (0.08f, oxide * (0.25f + 0.75f * oxideMod)));
        const float oxideCoeff = std::exp (-2.0f * juce::MathConstants<float>::pi
                                           * oxideCutoff / (float) sampleRate);

        // ---- wow / flutter ---------------------------------------------------
        const bool useWow = wow > 0.001f;
        const float baseDelaySec    = 0.003f + wear * 0.002f;
        const float wowDepthSec     = wow * 0.0025f;
        const float flutterDepthSec = wow * 0.00035f;

        const double wowInc     = 2.0 * juce::MathConstants<double>::pi * 0.33 / sampleRate;
        const double flutterInc = 2.0 * juce::MathConstants<double>::pi * 4.70 / sampleRate;
        const double breatheInc = 2.0 * juce::MathConstants<double>::pi * 0.11 / sampleRate;

        const float dropoutChance = dropouts * (0.20f + 0.80f * wear) * 0.000025f;

        // Weight for phrase-quantised dropouts: strongest on the boundary,
        // weakest in the middle of a phrase.
        float phraseWeight = 1.0f;
        if (p.phrasePhase >= 0.0f)
        {
            const float ph = p.phrasePhase - std::floor (p.phrasePhase);
            const float edge = juce::jmin (ph, 1.0f - ph) * 2.0f;   // 0 at edge, 1 mid
            phraseWeight = 0.15f + 2.6f * (1.0f - edge) * (1.0f - edge);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            float xl = L[i];
            float xr = R[i];

            // ---- wow / flutter (modulated delay) ----------------------------
            delayL[(size_t) delayWrite] = xl;
            delayR[(size_t) delayWrite] = xr;

            if (useWow)
            {
                const float modL = std::sin ((float) wowPhase) * wowDepthSec
                                 + std::sin ((float) flutterPhase) * flutterDepthSec;
                const float modR = std::sin ((float) wowPhase + 1.1f) * wowDepthSec
                                 + std::sin ((float) flutterPhase + 0.7f) * flutterDepthSec;

                const float dl = juce::jlimit (1.0f, (float) delaySize - 2.0f,
                                               (baseDelaySec + modL) * (float) sampleRate);
                const float dr = juce::jlimit (1.0f, (float) delaySize - 2.0f,
                                               (baseDelaySec + modR) * (float) sampleRate);

                xl = readDelay (delayL, dl);
                xr = readDelay (delayR, dr);
            }

            delayWrite = (delayWrite + 1) % delaySize;

            wowPhase += wowInc;
            flutterPhase += flutterInc;
            if (wowPhase >= 2.0 * juce::MathConstants<double>::pi)     wowPhase -= 2.0 * juce::MathConstants<double>::pi;
            if (flutterPhase >= 2.0 * juce::MathConstants<double>::pi) flutterPhase -= 2.0 * juce::MathConstants<double>::pi;

            // ---- tone LP ----------------------------------------------------
            lpL = (1.0f - toneCoeff) * xl + toneCoeff * lpL;
            lpR = (1.0f - toneCoeff) * xr + toneCoeff * lpR;
            xl = lpL;
            xr = lpR;

            // ---- oxide LP ---------------------------------------------------
            if (oxide > 0.001f)
            {
                oxideL = (1.0f - oxideCoeff) * xl + oxideCoeff * oxideL;
                oxideR = (1.0f - oxideCoeff) * xr + oxideCoeff * oxideR;
                xl = oxideL;
                xr = oxideR;
            }
            else
            {
                oxideL = xl;
                oxideR = xr;
            }

            // ---- dropouts ---------------------------------------------------
            if (dropoutCooldown > 0)
                --dropoutCooldown;

            if (dropoutRemaining <= 0 && dropoutCooldown <= 0 && dropoutChance > 0.0f)
            {
                if (rng.nextFloat() < dropoutChance * phraseWeight)
                {
                    dropoutRemaining = (int) ((0.010f + rng.nextFloat() * 0.120f)
                                              * (float) sampleRate);
                    holding = rng.nextFloat() < 0.35f;
                    holdL = xl;
                    holdR = xr;
                    dropoutTarget = 0.0f;
                }
            }

            if (dropoutRemaining > 0)
            {
                --dropoutRemaining;

                if (holding)
                {
                    xl = holdL;
                    xr = holdR;
                }

                if (dropoutRemaining <= 0)
                {
                    dropoutTarget = 1.0f;
                    dropoutCooldown = (int) (0.05 * sampleRate);
                }
            }

            const float dCoeff = (dropoutTarget < dropoutGain) ? dropoutFallCoeff
                                                               : dropoutRiseCoeff;
            dropoutGain += dCoeff * (dropoutTarget - dropoutGain);
            xl *= dropoutGain;
            xr *= dropoutGain;

            // ---- saturation --------------------------------------------------
            if (sat > 0.001f)
            {
                const float drive = 1.0f + sat * 4.0f;
                const float comp  = 1.0f / (1.0f + sat * 1.2f);
                xl = std::tanh (xl * drive) * comp;
                xr = std::tanh (xr * drive) * comp;
            }

            // ---- hiss --------------------------------------------------------
            if (hiss > 0.001f)
            {
                const float noiseL = rng.nextFloat() * 2.0f - 1.0f;
                const float noiseR = rng.nextFloat() * 2.0f - 1.0f;

                hissLpL += 0.30f * (noiseL - hissLpL);
                hissLpR += 0.30f * (noiseR - hissLpR);

                envL = juce::jmax (std::abs (xl), envL * envelopeCoeff);
                envR = juce::jmax (std::abs (xr), envR * envelopeCoeff);

                const float gateL = 0.08f + 0.92f * juce::jmin (1.0f, envL * 2.0f);
                const float gateR = 0.08f + 0.92f * juce::jmin (1.0f, envR * 2.0f);

                xl += hissLpL * hiss * 0.10f * gateL;
                xr += hissLpR * hiss * 0.10f * gateR;
            }

            // ---- wear gain + slow "breathing" --------------------------------
            const float wearGain = 1.0f - wear * 0.25f;
            const float breathe  = 1.0f + wear * 0.06f * std::sin ((float) breathePhase);
            breathePhase += breatheInc;
            if (breathePhase >= 2.0 * juce::MathConstants<double>::pi)
                breathePhase -= 2.0 * juce::MathConstants<double>::pi;

            xl *= wearGain * breathe;
            xr *= wearGain * breathe;

            // ---- DC blocker ---------------------------------------------------
            const float yl = xl - dcXL + 0.995f * dcYL;
            const float yr = xr - dcXR + 0.995f * dcYR;
            dcXL = xl; dcYL = yl;
            dcXR = xr; dcYR = yr;

            L[i] = std::isfinite (yl) ? yl : 0.0f;
            R[i] = std::isfinite (yr) ? yr : 0.0f;
        }
    }

    float getDropoutGain() const noexcept { return dropoutGain; }

private:
    float readDelay (const std::vector<float>& buf, float delaySamples) const noexcept
    {
        float pos = (float) delayWrite - delaySamples;
        while (pos < 0.0f)
            pos += (float) delaySize;

        const int i0 = (int) pos;
        const float frac = pos - (float) i0;
        const int i1 = (i0 + 1) % delaySize;

        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * frac;
    }

    double sampleRate = 48000.0;
    int    delaySize = 0;

    std::vector<float> delayL, delayR;
    int delayWrite = 0;

    double wowPhase = 0.0, flutterPhase = 0.0, breathePhase = 0.0;

    float lpL = 0.0f, lpR = 0.0f;
    float oxideL = 0.0f, oxideR = 0.0f;
    float hissLpL = 0.0f, hissLpR = 0.0f;

    float dropoutGain = 1.0f;
    float dropoutTarget = 1.0f;
    int   dropoutRemaining = 0;
    int   dropoutCooldown = 0;
    bool  holding = false;
    float holdL = 0.0f, holdR = 0.0f;

    float dropoutFallCoeff = 0.0f, dropoutRiseCoeff = 0.0f;
    float oxideSmoothCoeff = 0.0f, envelopeCoeff = 0.0f;

    float oxideMod = 0.0f, oxideTarget = 0.0f;
    int   oxideTimer = 0;

    float envL = 0.0f, envR = 0.0f;
    float dcXL = 0.0f, dcXR = 0.0f, dcYL = 0.0f, dcYR = 0.0f;
};

} // namespace somnia
