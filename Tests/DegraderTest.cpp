// Offline test for the Somnia tape degrader: clean path, wear darkening,
// dropouts, hiss, wow, saturation and non-finite robustness.

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "../Source/dsp/TapeDegrader.h"

namespace
{
    float rms (const std::vector<float>& v)
    {
        double acc = 0.0;
        for (float x : v)
            acc += (double) x * x;
        return (float) std::sqrt (acc / (double) juce::jmax ((size_t) 1, v.size()));
    }

    bool finite (const std::vector<float>& v)
    {
        for (float x : v)
            if (! std::isfinite (x))
                return false;
        return true;
    }

    void sine (std::vector<float>& l, std::vector<float>& r, double sr, double hz)
    {
        for (size_t i = 0; i < l.size(); ++i)
        {
            const float s = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi
                                                     * hz * (double) i / sr));
            l[i] = s;
            r[i] = s;
        }
    }
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr int n = 48000;   // 1 second

    somnia::TapeDegrader deg;
    deg.prepare (sr);

    juce::Random rng (0xdecaf);

    std::vector<float> inL (n), inR (n), outL (n), outR (n);

    // --- clean path ----------------------------------------------------------
    sine (inL, inR, sr, 440.0);
    outL = inL;
    outR = inR;

    somnia::TapeDegrader::Params clean;
    clean.wear = 0.0f;
    clean.dropouts = 0.0f;
    clean.oxide = 0.0f;
    clean.wow = 0.0f;
    clean.hiss = 0.0f;
    clean.tone = 1.0f;
    clean.sat = 0.0f;

    deg.process (outL.data(), outR.data(), n, clean, rng);

    const float inRms = rms (inL);
    const float cleanRms = rms (outL);
    const bool cleanOk = finite (outL) && std::abs (cleanRms / inRms - 1.0f) < 0.05f;

    // --- wear / oxide darken the top end ------------------------------------
    std::vector<float> hiL (n), hiR (n);
    sine (hiL, hiR, sr, 8000.0);

    outL = hiL;
    outR = hiR;

    somnia::TapeDegrader::Params worn;
    worn.wear = 1.0f;
    worn.oxide = 1.0f;
    worn.tone = 0.25f;
    worn.dropouts = 0.0f;
    worn.wow = 0.0f;
    worn.hiss = 0.0f;
    worn.sat = 0.0f;

    deg.process (outL.data(), outR.data(), n, worn, rng);

    const float hiInRms = rms (hiL);
    const float wornRms = rms (outL);
    const bool darkOk = finite (outL) && wornRms < hiInRms * 0.35f;

    // --- dropouts ------------------------------------------------------------
    sine (inL, inR, sr, 440.0);
    outL = inL;
    outR = inR;

    somnia::TapeDegrader::Params dropping;
    dropping.wear = 1.0f;
    dropping.dropouts = 1.0f;
    dropping.oxide = 0.0f;
    dropping.wow = 0.0f;
    dropping.hiss = 0.0f;
    dropping.tone = 1.0f;
    dropping.sat = 0.0f;

    float minGain = 1.0f;
    for (int b = 0; b < 10; ++b)
    {
        deg.process (outL.data(), outR.data(), n, dropping, rng);
        minGain = juce::jmin (minGain, deg.getDropoutGain());
    }
    const bool dropoutOk = finite (outL) && minGain < 0.25f;

    // --- hiss on silence -----------------------------------------------------
    std::vector<float> sil (n, 0.0f);
    outL = sil;
    outR = sil;

    somnia::TapeDegrader::Params noisy;
    noisy.wear = 0.0f;
    noisy.dropouts = 0.0f;
    noisy.oxide = 0.0f;
    noisy.wow = 0.0f;
    noisy.hiss = 1.0f;
    noisy.tone = 1.0f;
    noisy.sat = 0.0f;

    deg.process (outL.data(), outR.data(), n, noisy, rng);
    const bool hissOk = finite (outL) && rms (outL) > 0.001f;

    // --- wow / flutter and saturation ---------------------------------------
    sine (inL, inR, sr, 440.0);
    outL = inL;
    outR = inR;

    somnia::TapeDegrader::Params extreme;
    extreme.wear = 0.8f;
    extreme.dropouts = 0.6f;
    extreme.oxide = 0.8f;
    extreme.wow = 1.0f;
    extreme.hiss = 0.4f;
    extreme.tone = 0.2f;
    extreme.sat = 1.0f;

    bool extremeFinite = true;
    float extremeMax = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        deg.process (outL.data(), outR.data(), n, extreme, rng);
        extremeFinite = extremeFinite && finite (outL);
        for (float x : outL)
            extremeMax = juce::jmax (extremeMax, std::abs (x));
    }
    const bool extremeOk = extremeFinite && extremeMax < 4.0f;

    // --- non-finite input ----------------------------------------------------
    outL.assign (n, std::numeric_limits<float>::quiet_NaN());
    outR.assign (n, std::numeric_limits<float>::infinity());
    deg.process (outL.data(), outR.data(), n, extreme, rng);
    const bool nanOk = finite (outL);

    std::cout << "cleanRms=" << cleanRms << " wornRms=" << wornRms
              << " minGain=" << minGain << " extremeMax=" << extremeMax
              << " clean=" << cleanOk << " dark=" << darkOk
              << " drop=" << dropoutOk << " hiss=" << hissOk
              << " extreme=" << extremeOk << " nan=" << nanOk << std::endl;

    if (! (cleanOk && darkOk && dropoutOk && hissOk && extremeOk && nanOk))
    {
        std::cout << "FAIL: tape degrader" << std::endl;
        return 1;
    }

    std::cout << "PASS: tape degrader" << std::endl;
    return 0;
}
