#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace somnia
{

//==============================================================================
//  The physical tape loop.
//
//  A stereo circular buffer (up to 60 seconds) with one record head and one
//  play head.  The play head can run forward, backward, ping-pong or choose a
//  random direction on every pass; its speed is the varispeed control, so
//  slowing the tape down drops the pitch exactly like a real cassette.
//
//  The loop is deliberately *not* a clean sampler: the read head may jump
//  (splices), the record head writes the degraded signal back into the tape
//  (overdub / generation loss), and the loop length can either follow the host
//  tempo or be free (record until STOP).
//==============================================================================
class TapeLoop
{
public:
    enum Direction
    {
        forward  = 0,
        reverse  = 1,
        pingPong = 2,
        random   = 3
    };

    struct Params
    {
        float speed      = 1.0f;    // playback rate (1.0 = normal)
        float feedback   = 0.55f;   // how much degraded loop is written back
        int   direction  = forward;
        bool  playing    = false;
        bool  recording  = false;
        bool  overdub    = true;
        float splice     = 0.0f;    // 0..1 read-head jump probability
        float phraseSeconds = 0.0f; // musical phrase length for splices (0 = loop / 2)
    };

    //==========================================================================
    void prepare (double newSampleRate, double maxSeconds = 60.0)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        maxSamples = (int) std::ceil (maxSeconds * sampleRate);
        maxSamples = juce::jmax (maxSamples, (int) sampleRate);   // at least 1 s

        bufferL.assign ((size_t) maxSamples, 0.0f);
        bufferR.assign ((size_t) maxSamples, 0.0f);

        fadeStep = 1.0f / (float) juce::jmax (1.0, 0.004 * sampleRate);   // 4 ms
        reset();
    }

    void reset()
    {
        std::fill (bufferL.begin(), bufferL.end(), 0.0f);
        std::fill (bufferR.begin(), bufferR.end(), 0.0f);

        loopLength   = 0.0;
        tapeLength   = 0.0;
        loopStart    = 0.0;
        targetLength = 0.0;
        writePos     = 0.0;
        readPos      = 0.0;
        dir          = 1;
        lastDirection = forward;
        playing      = false;
        recording    = false;
        overdubMode  = true;
        freshRecording = false;
        recordedOnce = false;
        tapeHasContent = false;
        freeMode     = true;
        cycleCount   = 0;
        wrapFlag     = false;
        fadeGain     = 1.0f;
        spliceCooldown = 0;
        lastPhraseIndex = -1;
        lastPhraseLen = 0.0;
    }

    //==========================================================================
    void setFreeMode (bool shouldBeFree) noexcept
    {
        freeMode = shouldBeFree;
    }

    bool isFreeMode() const noexcept { return freeMode; }

    // Length in seconds for the tempo-synced modes.  Ignored while a free
    // recording is in progress.  Resets the loop window to the whole tape.
    void setLoopLengthSeconds (double seconds)
    {
        const double maxSec = (double) maxSamples / sampleRate;
        targetLength = juce::jlimit (0.05, maxSec, seconds) * sampleRate;
        targetLength = juce::jmin (targetLength, (double) maxSamples);

        if (freeMode || freshRecording)
            return;

        tapeLength = targetLength;
        loopStart = 0.0;
        loopLength = tapeLength;
        wrapPositions();
    }

    // Sets the amount of recorded material without touching the loop window.
    // Used by the tempo-synced mode so the user's visual loop range survives a
    // BPM change (the range is stored as a fraction of the tape length).
    void setTapeLengthSeconds (double seconds)
    {
        const double maxSec = (double) maxSamples / sampleRate;
        const double newLength = juce::jlimit (0.05, maxSec, seconds) * sampleRate;

        if (std::abs (newLength - tapeLength) < 0.5)
            return;

        tapeLength = juce::jmin (newLength, (double) maxSamples);
        clampLoopBounds();
        wrapPositions();
    }

    // Loop window inside the recorded tape.  Both values are in seconds and are
    // clamped to [0, tapeLength]; the minimum window is ~20 ms.
    void setLoopBoundsSeconds (double startSeconds, double endSeconds)
    {
        if (tapeLength <= 0.0 || ! tapeHasContent)
            return;

        const double tapeSec = tapeLength / sampleRate;
        double s = juce::jlimit (0.0, tapeSec, startSeconds) * sampleRate;
        double e = juce::jlimit (0.0, tapeSec, endSeconds) * sampleRate;

        // A full-range request (the common case while a free recording grows)
        // simply follows the tape end; do not reset the fade every block.
        if (s <= 0.5 && e >= tapeLength - 0.5)
        {
            const double oldStart  = loopStart;
            const double oldLength = loopLength;

            loopStart = 0.0;
            loopLength = tapeLength;

            // While a free recording grows, the write head sits exactly at the
            // tape end; wrapping it would rewind the take to the start.
            keepHeadsInWindow (oldStart, oldLength, ! (freeMode && freshRecording));
            return;
        }

        const double minLen = (double) minLoopSamples();
        if (e - s < minLen)
        {
            if (s + minLen <= tapeLength)
                e = s + minLen;
            else
            {
                e = tapeLength;
                s = juce::jmax (0.0, e - minLen);
            }
        }

        if (std::abs (s - loopStart) < 0.5
            && std::abs ((e - s) - loopLength) < 0.5)
            return;

        const double oldStart  = loopStart;
        const double oldLength = loopLength;

        loopStart = s;
        loopLength = e - s;

        keepHeadsInWindow (oldStart, oldLength, true);
    }

    double getLoopLengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? loopLength / sampleRate : 0.0;
    }

    double getLoopStartSeconds() const noexcept
    {
        return sampleRate > 0.0 ? loopStart / sampleRate : 0.0;
    }

    double getLoopEndSeconds() const noexcept
    {
        return sampleRate > 0.0 ? (loopStart + loopLength) / sampleRate : 0.0;
    }

    double getTapeLengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? tapeLength / sampleRate : 0.0;
    }

    double getTargetLengthSeconds() const noexcept
    {
        return sampleRate > 0.0 ? targetLength / sampleRate : 0.0;
    }

    double getPositionNormalised() const noexcept
    {
        return loopLength > 0.0 ? juce::jlimit (0.0, 1.0, readPos / loopLength) : 0.0;
    }

    int getDirectionSign() const noexcept { return dir; }

    //==========================================================================
    void startPlayback() noexcept
    {
        playing = true;
    }

    void stopPlayback() noexcept
    {
        playing = false;
    }

    // Starts recording.  With overdub = false the old tape is erased from the
    // top; with overdub = true the input is summed into the existing loop.
    void startRecording (bool overdub)
    {
        playing = true;
        recording = true;
        overdubMode = overdub;

        if (! overdub || loopLength < (double) minLoopSamples())
        {
            freshRecording = true;
            tapeHasContent = false;
            writePos = 0.0;
            readPos  = 0.0;
            dir      = 1;
            cycleCount = 0;

            if (freeMode)
            {
                tapeLength = 0.0;
                loopStart  = 0.0;
                loopLength = 0.0;
            }
            else
            {
                if (tapeLength <= 0.0)
                    tapeLength = targetLength > 0.0
                                     ? targetLength
                                     : (double) juce::jmax (minLoopSamples(),
                                                            (int) (2.0 * sampleRate));

                loopStart = 0.0;
                loopLength = tapeLength;
                targetLength = tapeLength;
            }
        }
        else
        {
            freshRecording = false;
        }
    }

    void stopRecording()
    {
        if (! recording)
            return;

        recording = false;

        if (freeMode && freshRecording)
        {
            if (writePos >= (double) minLoopSamples())
            {
                tapeLength = writePos;
                loopStart = 0.0;
                loopLength = tapeLength;
                recordedOnce = true;
            }
            else
            {
                tapeLength = 0.0;
                loopStart = 0.0;
                loopLength = 0.0;
                tapeHasContent = false;
            }
        }

        freshRecording = false;
    }

    void clear()
    {
        std::fill (bufferL.begin(), bufferL.end(), 0.0f);
        std::fill (bufferR.begin(), bufferR.end(), 0.0f);
        tapeLength = 0.0;
        loopStart = 0.0;
        loopLength = 0.0;
        writePos = 0.0;
        readPos = 0.0;
        cycleCount = 0;
        recordedOnce = false;
        tapeHasContent = false;
        freshRecording = false;
        recording = false;
        playing = false;
        wrapFlag = false;
        fadeGain = 1.0f;
        spliceCooldown = 0;
        lastPhraseIndex = -1;
        lastPhraseLen = 0.0;
    }

    //==========================================================================
    void rewind (double seconds)
    {
        if (loopLength <= 0.0)
            return;

        readPos -= seconds * sampleRate;
        wrapReadPosition();
        fadeGain = 0.0f;
    }

    void fastForward (double seconds)
    {
        if (loopLength <= 0.0)
            return;

        readPos += seconds * sampleRate;
        wrapReadPosition();
        fadeGain = 0.0f;
    }

    //==========================================================================
    bool isPlaying() const noexcept    { return playing; }
    bool isRecording() const noexcept  { return recording; }

    // True while a brand-new take is being written (no material before it).
    // The processor uses this to keep the dry path untouched: during a fresh
    // recording the wet signal is just the input being written, so mixing it
    // in would only attenuate the dry signal.
    bool isFreshRecording() const noexcept { return recording && freshRecording; }

    bool hasContent() const noexcept   { return tapeHasContent
                                                && loopLength >= (double) minLoopSamples(); }
    int  getCycleCount() const noexcept { return cycleCount; }

    // Bumped whenever new material lands on the tape.  The tape map uses it to
    // know the waveform changed: in synced mode the tape length is fixed from
    // the start, so watching the length alone never noticed a growing take.
    std::uint32_t getWriteVersion() const noexcept
    {
        return writeVersion.load (std::memory_order_relaxed);
    }

    // Fills minOut/maxOut with a decimated min/max envelope of the whole
    // recorded tape.  Called from the message thread for the loop editor; the
    // buffer is only ever resized in prepare(), so this is safe enough for a
    // visualisation (a sample may be caught mid-write, which is harmless).
    void copyWaveform (float* minOut, float* maxOut, int numBuckets) const
    {
        if (minOut == nullptr || maxOut == nullptr || numBuckets <= 0)
            return;

        const int n = juce::jlimit (0, maxSamples, (int) tapeLength);

        for (int b = 0; b < numBuckets; ++b)
        {
            const int s0 = (int) ((juce::int64) b * n / numBuckets);
            const int s1 = (int) ((juce::int64) (b + 1) * n / numBuckets);

            if (s1 <= s0)
            {
                minOut[b] = 0.0f;
                maxOut[b] = 0.0f;
                continue;
            }

            const int stride = juce::jmax (1, (s1 - s0) / 12);
            float mn = 1.0e9f, mx = -1.0e9f;

            for (int i = s0; i < s1; i += stride)
            {
                const float v = 0.5f * (bufferL[(size_t) i] + bufferR[(size_t) i]);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }

            const float last = 0.5f * (bufferL[(size_t) (s1 - 1)]
                                       + bufferR[(size_t) (s1 - 1)]);
            mn = juce::jmin (mn, last);
            mx = juce::jmax (mx, last);

            minOut[b] = mn;
            maxOut[b] = mx;
        }
    }

    bool consumeWrapFlag() noexcept
    {
        const bool wasWrapped = wrapFlag;
        wrapFlag = false;
        return wasWrapped;
    }

    //==========================================================================
    // Reads the play head into outL/outR.  Never allocates.
    void read (float* outL, float* outR, int numSamples, const Params& p,
               juce::Random& rng)
    {
        if (outL == nullptr || outR == nullptr)
            return;

        // A fresh recording writes a brand-new take; there is nothing musical to
        // read back yet.  Reading here used to wrap the head around the growing
        // tape (resetting the pass counter) and to feed a half-written buffer
        // into the wet path, which made the output quieter while recording.
        // p.playing lets the processor keep the head running for the ADSR
        // release tail after STOP; the member flag is the tape's own state.
        if (! (playing || p.playing) || loopLength < (double) minLoopSamples()
            || (recording && freshRecording))
        {
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = 0.0f;
                outR[i] = 0.0f;
            }
            return;
        }

        const double len   = loopLength;
        const double start = loopStart;
        const double speed = juce::jlimit (0.25, 4.0, (double) p.speed);
        const int requestedDirection = juce::jlimit (0, 3, p.direction);

        if (requestedDirection != lastDirection)
        {
            lastDirection = requestedDirection;
            dir = requestedDirection == reverse ? -1 : 1;
            fadeGain = 0.0f;
        }

        const float spliceAmount = juce::jlimit (0.0f, 1.0f, p.splice);

        // Musical splices: the head only jumps on phrase boundaries, and it
        // keeps its phase inside the phrase, so a jump repeats or skips a
        // musical unit instead of landing on a random sample.  A phrase is one
        // bar from the host tempo (or half the loop when the loop is shorter).
        double phraseLen = len * 0.5;
        if (p.phraseSeconds > 0.0f)
            phraseLen = juce::jlimit ((double) minLoopSamples(), len,
                                      (double) p.phraseSeconds * sampleRate);

        int phraseDiv = (int) std::round (len / juce::jmax (1.0, phraseLen));
        if (phraseDiv < 2)
        {
            phraseDiv = 2;
            phraseLen = len * 0.5;
        }
        phraseLen = len / (double) phraseDiv;

        if (std::abs (phraseLen - lastPhraseLen) > 0.5)
        {
            lastPhraseLen = phraseLen;
            lastPhraseIndex = -1;
        }

        const float spliceChance = spliceAmount > 0.0f
                                       ? juce::jlimit (0.0f, 0.80f,
                                                       spliceAmount * 1.5f / (float) phraseDiv)
                                       : 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            if (requestedDirection == forward)
                dir = 1;
            else if (requestedDirection == reverse)
                dir = -1;

            const float l = readCubic (bufferL, start + readPos, start, len);
            const float r = readCubic (bufferR, start + readPos, start, len);

            outL[i] = l * fadeGain;
            outR[i] = r * fadeGain;

            if (fadeGain < 1.0f)
                fadeGain = juce::jmin (1.0f, fadeGain + fadeStep);

            readPos += speed * (double) dir;

            if (requestedDirection == pingPong)
            {
                // Reflect at the tape ends instead of wrapping.  Wrapping and
                // flipping the direction at the same time made the head bounce
                // between the two edge samples (and the cycle counter explode).
                if (readPos >= len)
                {
                    readPos = 2.0 * len - readPos;
                    dir = -1;
                    onWrap();
                }
                else if (readPos < 0.0)
                {
                    readPos = -readPos;
                    dir = 1;
                    onWrap();
                }

                // Never clamp the head to the window edge.  Clamping to len-1
                // stopped the head *before* it could cross the boundary at a
                // fractional speed (0.5, 0.25), so ping-pong froze against the
                // right edge after the first pass.  A reflection can only
                // overshoot when the step is larger than the whole window, so
                // the fallback below is a safety net, not the normal path.
                if (! std::isfinite (readPos) || readPos >= len || readPos < 0.0)
                {
                    readPos = std::isfinite (readPos) ? std::fmod (readPos, len) : 0.0;
                    if (readPos < 0.0)
                        readPos += len;
                }
            }
            else if (readPos >= len)
            {
                readPos -= len;
                onWrap (requestedDirection, rng);
            }
            else if (readPos < 0.0)
            {
                readPos += len;
                onWrap (requestedDirection, rng);
            }

            // Splices: jump to another phrase boundary.  The jump happens only
            // when the head crosses a boundary, which keeps every cut on the
            // musical grid; a short fade masks the discontinuity.
            if (spliceChance > 0.0f)
            {
                const int phraseIndex = (int) std::floor (readPos / phraseLen);

                if (lastPhraseIndex < 0)
                {
                    lastPhraseIndex = phraseIndex;
                }
                else if (phraseIndex != lastPhraseIndex)
                {
                    lastPhraseIndex = phraseIndex;

                    if (spliceCooldown <= 0 && rng.nextFloat() < spliceChance)
                    {
                        const double phase = readPos - (double) phraseIndex * phraseLen;

                        // Repeat or skip one or two phrases, keeping the beat.
                        int target = phraseIndex
                                     + (rng.nextBool() ? 1 : -1) * (1 + rng.nextInt (2));
                        target %= phraseDiv;
                        if (target < 0)
                            target += phraseDiv;

                        readPos = (double) target * phraseLen + phase;
                        if (readPos >= len)
                            readPos -= len;
                        if (readPos < 0.0)
                            readPos += len;

                        fadeGain = 0.0f;
                        spliceCooldown = (int) (0.08 * sampleRate);
                    }
                }
            }

            if (spliceCooldown > 0)
                --spliceCooldown;
        }
    }

    //==========================================================================
    // Writes input + feedback * degraded loop into the tape.
    // fbL/fbR is the already-degraded loop signal (no ADSR applied).
    void write (const float* inL, const float* inR,
                const float* fbL, const float* fbR,
                int numSamples, const Params& p)
    {
        if (! recording)
            return;

        // A free recording starts with an empty tape (loopLength == 0); the
        // write head grows the loop as it goes, so this is the one case where
        // writing before the minimum length is valid.
        if (loopLength < (double) minLoopSamples() && ! (freeMode && freshRecording))
            return;

        const float fb = juce::jlimit (0.0f, 0.95f, p.feedback);

        for (int i = 0; i < numSamples; ++i)
        {
            const int wi = juce::jlimit (0, maxSamples - 1,
                                         (int) (loopStart + writePos));

            const float inl = inL != nullptr ? inL[i] : 0.0f;
            const float inr = inR != nullptr ? inR[i] : 0.0f;

            float nl = inl;
            float nr = inr;

            if (overdubMode && fb > 0.0f)
            {
                const float oldL = fbL != nullptr ? fbL[i] : bufferL[(size_t) wi];
                const float oldR = fbR != nullptr ? fbR[i] : bufferR[(size_t) wi];
                nl += fb * oldL;
                nr += fb * oldR;
            }

            if (! std::isfinite (nl)) nl = 0.0f;
            if (! std::isfinite (nr)) nr = 0.0f;

            bufferL[(size_t) wi] = softClip (nl);
            bufferR[(size_t) wi] = softClip (nr);
            tapeHasContent = true;

            if (freeMode && freshRecording)
            {
                writePos += 1.0;
                tapeLength = juce::jmin ((double) maxSamples, writePos);
                loopLength = tapeLength;

                if (writePos >= (double) maxSamples)
                {
                    // The tape is full: stop automatically.
                    recording = false;
                    freshRecording = false;
                    recordedOnce = true;
                    break;
                }
            }
            else
            {
                writePos += 1.0;
                if (writePos >= loopLength)
                    writePos -= loopLength;
            }
        }

        writeVersion.fetch_add (1, std::memory_order_relaxed);
    }

private:
    //==========================================================================
    int minLoopSamples() const noexcept
    {
        return juce::jmax (64, (int) (0.02 * sampleRate));
    }

    void onWrap (int requestedDirection, juce::Random& rng) noexcept
    {
        ++cycleCount;
        wrapFlag = true;

        // Ping-pong direction is handled by the reflection above; random picks
        // a fresh direction for the next pass.
        if (requestedDirection == random)
            dir = rng.nextBool() ? 1 : -1;
    }

    void onWrap() noexcept
    {
        ++cycleCount;
        wrapFlag = true;
    }

    void wrapPositions() noexcept
    {
        if (loopLength <= 0.0)
            return;

        readPos = std::fmod (readPos, loopLength);
        if (readPos < 0.0)
            readPos += loopLength;

        writePos = std::fmod (writePos, loopLength);
        if (writePos < 0.0)
            writePos += loopLength;
    }

    // Applies a change of the loop window without moving the material under the
    // heads.  readPos/writePos are relative to loopStart, so a window move has
    // to be compensated; otherwise every drag step plays a different part of
    // the tape.  The fade is only restarted on a real jump: a smooth drag
    // changes the window once per block, and resetting the gain that often
    // turned into an audible amplitude modulation ("stuttering" while a marker
    // was dragged).
    void keepHeadsInWindow (double oldStart, double oldLength, bool wrapWrite) noexcept
    {
        if (loopLength <= 0.0)
            return;

        readPos -= (loopStart - oldStart);

        const bool jumped = readPos < 0.0 || readPos >= loopLength;

        if (jumped)
        {
            readPos = std::fmod (readPos, loopLength);
            if (readPos < 0.0)
                readPos += loopLength;
        }

        if (wrapWrite)
        {
            writePos = std::fmod (writePos, loopLength);
            if (writePos < 0.0)
                writePos += loopLength;
        }

        if (jumped || std::abs (loopLength - oldLength) > 0.05 * sampleRate)
            fadeGain = 0.0f;
    }

    void wrapReadPosition() noexcept
    {
        if (loopLength <= 0.0)
            return;

        readPos = std::fmod (readPos, loopLength);
        if (readPos < 0.0)
            readPos += loopLength;
    }

    void clampLoopBounds() noexcept
    {
        if (tapeLength <= 0.0)
        {
            loopStart = 0.0;
            loopLength = 0.0;
            return;
        }

        // No loop window yet: keep it empty until a recording creates one.
        if (loopLength <= 0.0)
        {
            loopStart = 0.0;
            return;
        }

        loopStart = juce::jlimit (0.0, tapeLength, loopStart);
        loopLength = juce::jlimit (0.0, tapeLength - loopStart, loopLength);

        const double minLen = (double) minLoopSamples();
        if (loopLength < minLen)
        {
            loopLength = juce::jmin (minLen, tapeLength);
            loopStart = juce::jmin (loopStart, tapeLength - loopLength);
        }
    }

    static float readCubic (const std::vector<float>& buf, double pos,
                            double start, double len) noexcept
    {
        const int n = (int) len;
        if (n <= 0)
            return 0.0f;

        const int base = (int) start;
        const int i1 = (int) std::floor (pos);
        const float frac = (float) (pos - (double) i1);

        auto at = [n, base] (int i) noexcept
        {
            int rel = i - base;
            rel %= n;
            if (rel < 0)
                rel += n;
            return base + rel;
        };

        if (n < 4)
        {
            const float y0 = buf[(size_t) at (i1)];
            const float y1 = buf[(size_t) at (i1 + 1)];
            return y0 + (y1 - y0) * frac;
        }

        const float y0 = buf[(size_t) at (i1 - 1)];
        const float y1 = buf[(size_t) at (i1)];
        const float y2 = buf[(size_t) at (i1 + 1)];
        const float y3 = buf[(size_t) at (i1 + 2)];

        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    static float softClip (float x) noexcept
    {
        if (x > 1.0f)
            return 1.0f + std::tanh (x - 1.0f) * 0.5f;
        if (x < -1.0f)
            return -1.0f - std::tanh (-x - 1.0f) * 0.5f;
        return x;
    }

    //==========================================================================
    double sampleRate = 48000.0;
    int    maxSamples = 0;

    std::vector<float> bufferL, bufferR;

    double tapeLength   = 0.0;   // total recorded material
    double loopStart    = 0.0;   // offset of the loop window inside the tape
    double loopLength   = 0.0;   // length of the loop window
    double targetLength = 0.0;
    double writePos     = 0.0;
    double readPos      = 0.0;

    int  dir = 1;
    int  lastDirection = forward;

    bool playing         = false;
    bool recording       = false;
    bool overdubMode     = true;
    bool freshRecording  = false;
    bool recordedOnce    = false;
    bool tapeHasContent  = false;
    bool freeMode        = true;
    bool wrapFlag        = false;

    int   cycleCount     = 0;
    float fadeGain       = 1.0f;
    float fadeStep       = 0.0f;
    int   spliceCooldown = 0;

    int    lastPhraseIndex = -1;
    double lastPhraseLen   = 0.0;

    std::atomic<std::uint32_t> writeVersion { 0 };
};

} // namespace somnia
