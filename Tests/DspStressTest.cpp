// Offline stress test for the complete Somnia tape chain.
//
// Records a noisy loop, then runs tape -> degrader -> ADSR -> limiter for about
// 40 seconds with extreme and rapidly changing settings (maximum feedback,
// wear, dropouts, splices, oxide, wow, hiss, saturation, varispeed and all
// directions).  The output must stay finite, bounded and never run away.

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "../Source/dsp/TapeLoop.h"
#include "../Source/dsp/TapeDegrader.h"
#include "../Source/dsp/Adsr.h"
#include "../Source/dsp/Limiter.h"

int main()
{
    constexpr double sr = 48000.0;
    constexpr int block = 512;

    somnia::TapeLoop     tape;
    somnia::TapeDegrader degrader;
    somnia::Adsr         adsr;
    somnia::Limiter      limiter;

    tape.prepare (sr, 4.0);
    degrader.prepare (sr);
    adsr.prepare (sr);
    limiter.prepare (sr);

    juce::Random rng (0xbadc0de);

    std::vector<float> inL (block), inR (block);
    std::vector<float> loopL (block), loopR (block);
    std::vector<float> outL (block), outR (block);

    std::mt19937 noiseRng (12345);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);

    // --- record a noisy two-second loop --------------------------------------
    tape.setFreeMode (true);
    tape.startRecording (false);

    somnia::TapeLoop::Params tp;
    for (int b = 0; b < (int) (2.0 * sr / block); ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            inL[(size_t) i] = dist (noiseRng) * 0.5f;
            inR[(size_t) i] = dist (noiseRng) * 0.5f;
        }
        tape.write (inL.data(), inR.data(), nullptr, nullptr, block, tp);
    }
    tape.stopRecording();

    tape.setFreeMode (false);
    tape.setLoopLengthSeconds (2.0);
    tape.startRecording (true);   // overdub from now on: the loop ages

    adsr.setParams (0.005f, 0.050f, 0.90f, 0.050f);
    adsr.noteOn();

    double outMax = 0.0;
    double wetMax = 0.0;
    bool nonFinite = false;

    const int numBlocks = 4000;   // ~42 s at 48k

    for (int b = 0; b < numBlocks; ++b)
    {
        // Shake the parameters hard, like repeatedly hitting RANDOM.
        if (b % 250 == 0)
        {
            tp.direction = (b / 250) % 4;
            tp.speed = (b % 500 == 0) ? 4.0f : 0.25f;
            tp.feedback = 0.95f;
            tp.splice = 1.0f;
        }

        somnia::TapeDegrader::Params dp;
        dp.wear = 1.0f;
        dp.dropouts = 1.0f;
        dp.oxide = 1.0f;
        dp.wow = 1.0f;
        dp.hiss = 1.0f;
        dp.tone = 0.0f;
        dp.sat = 1.0f;

        for (int i = 0; i < block; ++i)
        {
            inL[(size_t) i] = dist (noiseRng) * 0.7f;
            inR[(size_t) i] = dist (noiseRng) * 0.7f;
        }

        tape.read (loopL.data(), loopR.data(), block, tp, rng);

        if (tape.consumeWrapFlag())
            adsr.noteOn();

        degrader.process (loopL.data(), loopR.data(), block, dp, rng);

        for (int i = 0; i < block; ++i)
        {
            const float e = adsr.process();
            outL[(size_t) i] = loopL[(size_t) i] * e;
            outR[(size_t) i] = loopR[(size_t) i] * e;
        }

        tape.write (inL.data(), inR.data(), loopL.data(), loopR.data(), block, tp);

        for (int i = 0; i < block; ++i)
        {
            if (! std::isfinite (outL[(size_t) i]) || ! std::isfinite (outR[(size_t) i]))
                nonFinite = true;

            wetMax = juce::jmax (wetMax, (double) std::abs (outL[(size_t) i]));
            wetMax = juce::jmax (wetMax, (double) std::abs (outR[(size_t) i]));
        }

        limiter.process (outL.data(), outR.data(), block, 0.98f);

        for (int i = 0; i < block; ++i)
        {
            if (! std::isfinite (outL[(size_t) i]) || ! std::isfinite (outR[(size_t) i]))
                nonFinite = true;

            outMax = juce::jmax (outMax, (double) std::abs (outL[(size_t) i]));
            outMax = juce::jmax (outMax, (double) std::abs (outR[(size_t) i]));
        }
    }

    // --- a NaN / Inf burst must not poison the loop --------------------------
    for (int i = 0; i < block; ++i)
    {
        inL[(size_t) i] = (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN()
                                       : std::numeric_limits<float>::infinity();
        inR[(size_t) i] = -std::numeric_limits<float>::infinity();
    }

    tape.write (inL.data(), inR.data(), loopL.data(), loopR.data(), block, tp);
    tape.read (loopL.data(), loopR.data(), block, tp, rng);
    degrader.process (loopL.data(), loopR.data(), block, somnia::TapeDegrader::Params{}, rng);
    limiter.process (loopL.data(), loopR.data(), block, 0.98f);

    bool nanOk = true;
    for (int i = 0; i < block; ++i)
        nanOk = nanOk && std::isfinite (loopL[(size_t) i])
                      && std::isfinite (loopR[(size_t) i]);

    const bool bounded = outMax <= 1.0 + 1.0e-3;

    std::cout << "outMax=" << outMax << " wetMax=" << wetMax
              << " nonFinite=" << nonFinite << " bounded=" << bounded
              << " nanOk=" << nanOk << std::endl;

    if (nonFinite || ! bounded || ! nanOk)
    {
        std::cout << "FAIL: DSP stress" << std::endl;
        return 1;
    }

    std::cout << "PASS: DSP stress" << std::endl;
    return 0;
}
