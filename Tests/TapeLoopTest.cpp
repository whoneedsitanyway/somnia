// Offline test for the Somnia tape loop: free and synced recording, playback,
// varispeed, all four directions, splices, clear and non-finite input.

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "../Source/dsp/TapeLoop.h"

namespace
{
    bool finiteAndBounded (const std::vector<float>& v, float limit = 2.0f)
    {
        for (float x : v)
            if (! std::isfinite (x) || std::abs (x) > limit)
                return false;
        return true;
    }

    float rms (const std::vector<float>& v)
    {
        double acc = 0.0;
        for (float x : v)
            acc += (double) x * x;
        return (float) std::sqrt (acc / (double) juce::jmax ((size_t) 1, v.size()));
    }
}

int main()
{
    constexpr double sr = 48000.0;
    constexpr int block = 512;

    somnia::TapeLoop tape;
    tape.prepare (sr, 4.0);
    tape.setFreeMode (true);

    juce::Random rng (0x5eed);

    std::vector<float> inL (block), inR (block);
    std::vector<float> outL (block), outR (block);

    auto fillSine = [&] (double phase)
    {
        for (int i = 0; i < block; ++i)
        {
            const float s = (float) (0.5 * std::sin (phase + 2.0 * juce::MathConstants<double>::pi
                                                              * 440.0 * (double) i / sr));
            inL[(size_t) i] = s;
            inR[(size_t) i] = s;
        }
    };

    somnia::TapeLoop::Params p;

    // --- free recording ------------------------------------------------------
    tape.startRecording (false);
    const int recordBlocks = (int) (sr / block);   // ~1 second
    double phase = 0.0;
    for (int b = 0; b < recordBlocks; ++b)
    {
        fillSine (phase);
        phase += 2.0 * juce::MathConstants<double>::pi * 440.0 * block / sr;
        tape.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
    }
    tape.stopRecording();

    const double freeLen = tape.getLoopLengthSeconds();
    const bool freeOk = tape.hasContent() && std::abs (freeLen - 1.0) < 0.02;

    // --- playback ------------------------------------------------------------
    tape.startPlayback();
    float peak = 0.0f;
    bool finite = true;
    for (int b = 0; b < recordBlocks; ++b)
    {
        tape.read (outL.data(), outR.data(), block, p, rng);
        finite = finite && finiteAndBounded (outL) && finiteAndBounded (outR);
        for (int i = 0; i < block; ++i)
            peak = juce::jmax (peak, std::abs (outL[(size_t) i]));
    }
    const bool playbackOk = finite && peak > 0.05f;

    // --- directions and varispeed -------------------------------------------
    bool dirsOk = true;
    for (int d = 0; d < 4; ++d)
    {
        p.direction = d;
        p.speed = (d == 0) ? 1.0f : (d == 1 ? 0.5f : 2.0f);
        p.splice = (d == 3) ? 1.0f : 0.0f;

        for (int b = 0; b < 64; ++b)
        {
            tape.read (outL.data(), outR.data(), block, p, rng);
            dirsOk = dirsOk && finiteAndBounded (outL) && finiteAndBounded (outR);
        }
    }

    const bool cyclesOk = tape.getCycleCount() > 0;

    // --- ping-pong must reflect at the ends, not bounce every sample ---------
    p.direction = somnia::TapeLoop::pingPong;
    p.speed = 1.0f;
    p.splice = 0.0f;
    const int cyclesBefore = tape.getCycleCount();
    double pingAcc = 0.0;
    int    pingCount = 0;
    for (int b = 0; b < 2 * recordBlocks; ++b)   // ~2 seconds
    {
        tape.read (outL.data(), outR.data(), block, p, rng);
        dirsOk = dirsOk && finiteAndBounded (outL) && finiteAndBounded (outR);

        for (int i = 0; i < block; ++i)
        {
            pingAcc += (double) outL[(size_t) i] * outL[(size_t) i];
            ++pingCount;
        }
    }
    const int pingPongCycles = tape.getCycleCount() - cyclesBefore;
    const bool pingPongOk = pingPongCycles > 0 && pingPongCycles < 12;

    // --- long ping-pong run: the level must not die after the first pass -----
    bool longPingOk = true;
    {
        somnia::TapeLoop t2;
        t2.prepare (sr, 4.0);
        t2.setFreeMode (true);
        t2.startRecording (false);

        double ph2 = 0.0;
        for (int b = 0; b < recordBlocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const float s = (float) (0.5 * std::sin (ph2 + 2.0 * juce::MathConstants<double>::pi
                                                              * 440.0 * (double) i / sr));
                inL[(size_t) i] = s;
                inR[(size_t) i] = s;
            }
            ph2 += 2.0 * juce::MathConstants<double>::pi * 440.0 * block / sr;
            t2.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
        }
        t2.stopRecording();
        t2.startPlayback();

        somnia::TapeLoop::Params pp;
        pp.direction = somnia::TapeLoop::pingPong;
        pp.speed = 1.0f;
        pp.splice = 0.0f;

        for (int sec = 0; sec < 8; ++sec)
        {
            double acc = 0.0;
            int n = 0;

            for (int b = 0; b < (int) (sr / block); ++b)
            {
                t2.read (outL.data(), outR.data(), block, pp, rng);
                for (int i = 0; i < block; ++i)
                {
                    acc += (double) outL[(size_t) i] * outL[(size_t) i];
                    ++n;
                }
            }

            const double r = std::sqrt (acc / (double) juce::jmax (1, n));
            if (r < 0.10)
            {
                longPingOk = false;
                std::cout << "long ping-pong sec=" << (sec + 1)
                          << " rms=" << r << std::endl;
            }
        }

        // Sweep the configurations a user can reach: varispeed, a narrow loop
        // window, splices and a phrase grid.
        for (float sp : { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f })
        {
            for (int narrow = 0; narrow < 2; ++narrow)
            {
                for (float spl : { 0.0f, 0.6f })
                {
                    somnia::TapeLoop t3;
                    t3.prepare (sr, 4.0);
                    t3.setFreeMode (true);
                    t3.startRecording (false);

                    double ph3 = 0.0;
                    for (int b = 0; b < recordBlocks; ++b)
                    {
                        for (int i = 0; i < block; ++i)
                        {
                            const float s = (float) (0.5 * std::sin (ph3 + 2.0 * juce::MathConstants<double>::pi
                                                                          * 440.0 * (double) i / sr));
                            inL[(size_t) i] = s;
                            inR[(size_t) i] = s;
                        }
                        ph3 += 2.0 * juce::MathConstants<double>::pi * 440.0 * block / sr;
                        t3.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
                    }
                    t3.stopRecording();

                    if (narrow != 0)
                        t3.setLoopBoundsSeconds (0.20, 0.55);

                    t3.startPlayback();

                    somnia::TapeLoop::Params q;
                    q.direction = somnia::TapeLoop::pingPong;
                    q.speed = sp;
                    q.splice = spl;
                    q.phraseSeconds = 0.25f;

                    double minRms = 1.0e9;

                    for (int sec = 0; sec < 6; ++sec)
                    {
                        double acc = 0.0;
                        int n = 0;

                        for (int b = 0; b < (int) (sr / block); ++b)
                        {
                            t3.read (outL.data(), outR.data(), block, q, rng);
                            for (int i = 0; i < block; ++i)
                            {
                                acc += (double) outL[(size_t) i] * outL[(size_t) i];
                                ++n;
                            }
                        }

                        const double secRms = std::sqrt (acc / (double) juce::jmax (1, n));
                        minRms = juce::jmin (minRms, secRms);
                    }

                    // A very slow head over a narrow window spends most of the
                    // pass near the edges, so the bar is lower there.
                    const double floorRms = (sp < 0.5f && narrow != 0) ? 0.02 : 0.10;

                    if (minRms < floorRms)
                    {
                        longPingOk = false;
                        std::cout << "sweep speed=" << sp << " narrow=" << narrow
                                  << " splice=" << spl << " minRms=" << minRms
                                  << std::endl;
                    }
                }
            }
        }
    }

    // A silent ping-pong pass would still be "finite and bounded", so check the
    // level explicitly: playback must not die when the direction is ping-pong.
    const float pingRms = (float) std::sqrt (pingAcc / (double) juce::jmax (1, pingCount));
    const bool pingAudible = pingRms > 0.05f;

    // --- synced length -------------------------------------------------------
    tape.clear();
    tape.setFreeMode (false);
    tape.setLoopLengthSeconds (0.5);
    tape.startRecording (false);
    for (int b = 0; b < recordBlocks; ++b)
    {
        fillSine (phase);
        phase += 2.0 * juce::MathConstants<double>::pi * 440.0 * block / sr;
        tape.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
    }
    tape.stopRecording();

    const double syncLen = tape.getLoopLengthSeconds();
    const bool syncOk = tape.hasContent() && std::abs (syncLen - 0.5) < 0.02;

    // --- visual loop bounds --------------------------------------------------
    tape.setLoopBoundsSeconds (0.10, 0.40);
    const bool boundsOk = std::abs (tape.getLoopStartSeconds() - 0.10) < 0.01
                       && std::abs (tape.getLoopEndSeconds() - 0.40) < 0.01
                       && std::abs (tape.getLoopLengthSeconds() - 0.30) < 0.01;

    tape.startPlayback();
    p.direction = somnia::TapeLoop::forward;
    p.speed = 1.0f;
    p.splice = 0.0f;
    for (int b = 0; b < 128; ++b)
    {
        tape.read (outL.data(), outR.data(), block, p, rng);
        finite = finite && finiteAndBounded (outL) && finiteAndBounded (outR);
    }

    // --- silence when stopped ------------------------------------------------
    tape.stopPlayback();
    tape.read (outL.data(), outR.data(), block, p, rng);
    bool silent = true;
    for (int i = 0; i < block; ++i)
        silent = silent && std::abs (outL[(size_t) i]) < 1.0e-7f
                        && std::abs (outR[(size_t) i]) < 1.0e-7f;

    // --- release tail --------------------------------------------------------
    // After STOP the processor keeps the head running for the ADSR release by
    // setting Params::playing; the tape itself is already stopped.
    tape.stopPlayback();
    p.playing = true;
    for (int b = 0; b < 4; ++b)
        tape.read (outL.data(), outR.data(), block, p, rng);

    float tailPeak = 0.0f;
    for (int i = 0; i < block; ++i)
        tailPeak = juce::jmax (tailPeak, std::abs (outL[(size_t) i]));
    const bool tailAudible = tailPeak > 0.05f;

    p.playing = false;
    tape.read (outL.data(), outR.data(), block, p, rng);
    bool tailSilent = true;
    for (int i = 0; i < block; ++i)
        tailSilent = tailSilent && std::abs (outL[(size_t) i]) < 1.0e-7f;

    // --- non-finite input ----------------------------------------------------
    tape.startRecording (false);
    for (int i = 0; i < block; ++i)
    {
        inL[(size_t) i] = std::numeric_limits<float>::quiet_NaN();
        inR[(size_t) i] = std::numeric_limits<float>::infinity();
    }
    tape.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
    tape.stopRecording();
    tape.startPlayback();
    tape.read (outL.data(), outR.data(), block, p, rng);
    const bool nanOk = finiteAndBounded (outL) && finiteAndBounded (outR);

    // --- clear ---------------------------------------------------------------
    tape.clear();
    const bool clearOk = ! tape.hasContent() && tape.getLoopLengthSeconds() == 0.0;

    // --- dragging a loop marker must not pump the level ---------------------
    // Every window update used to call wrapPositions() and reset the fade, so a
    // drag (one update per block) turned into amplitude modulation: the output
    // stuttered while a marker moved.
    bool dragOk = true;
    {
        somnia::TapeLoop t4;
        t4.prepare (sr, 4.0);
        t4.setFreeMode (true);
        t4.startRecording (false);

        double ph4 = 0.0;
        for (int b = 0; b < recordBlocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const float s = (float) (0.5 * std::sin (ph4 + 2.0 * juce::MathConstants<double>::pi
                                                              * 440.0 * (double) i / sr));
                inL[(size_t) i] = s;
                inR[(size_t) i] = s;
            }
            ph4 += 2.0 * juce::MathConstants<double>::pi * 440.0 * block / sr;
            t4.write (inL.data(), inR.data(), nullptr, nullptr, block, p);
        }
        t4.stopRecording();
        t4.setLoopBoundsSeconds (0.20, 0.80);
        t4.startPlayback();

        somnia::TapeLoop::Params q;
        q.direction = somnia::TapeLoop::forward;
        q.speed = 1.0f;

        auto measure = [&] (int blocks)
        {
            double acc = 0.0;
            int n = 0;

            for (int b = 0; b < blocks; ++b)
            {
                t4.read (outL.data(), outR.data(), block, q, rng);
                for (int i = 0; i < block; ++i)
                {
                    acc += (double) outL[(size_t) i] * outL[(size_t) i];
                    ++n;
                }
            }

            return std::sqrt (acc / (double) juce::jmax (1, n));
        };

        const double baseRms = measure (8);
        double worstRms = baseRms;

        // Slide the END marker in small steps, applying one update per block.
        for (int step = 0; step < 200; ++step)
        {
            const double end = 0.80 - 0.0015 * (double) step;
            t4.setLoopBoundsSeconds (0.20, end);
            worstRms = juce::jmin (worstRms, measure (1));
        }

        dragOk = baseRms > 0.1 && worstRms > baseRms * 0.7;

        if (! dragOk)
            std::cout << "marker drag baseRms=" << baseRms
                      << " worstRms=" << worstRms << std::endl;
    }

    std::cout << "freeLen=" << freeLen << " syncLen=" << syncLen
              << " peak=" << peak << " cycles=" << tape.getCycleCount()
              << " free=" << freeOk << " playback=" << playbackOk
              << " dirs=" << dirsOk << " pingpong=" << pingPongOk
              << " pingCycles=" << pingPongCycles
              << " pingRms=" << pingRms << " pingAudible=" << pingAudible
              << " longPing=" << longPingOk
              << " drag=" << dragOk
              << " sync=" << syncOk << " bounds=" << boundsOk
              << " silent=" << silent << " nan=" << nanOk
              << " tail=" << tailAudible << "/" << tailSilent
              << " clear=" << clearOk << std::endl;

    if (! (freeOk && playbackOk && dirsOk && cyclesOk && pingPongOk && pingAudible
           && longPingOk && dragOk
           && syncOk && boundsOk && silent && nanOk && tailAudible && tailSilent
           && clearOk))
    {
        std::cout << "FAIL: tape loop" << std::endl;
        return 1;
    }

    std::cout << "PASS: tape loop" << std::endl;
    return 0;
}
