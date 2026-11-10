#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
namespace
{
    using APVTS = juce::AudioProcessorValueTreeState;

}

//==============================================================================
SomniaAudioProcessor::SomniaAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    bindRawParams();
}

SomniaAudioProcessor::~SomniaAudioProcessor() = default;

//==============================================================================
void SomniaAudioProcessor::bindRawParams()
{
    using namespace somnia;
    auto bind = [this] (const char* id) { return apvts.getRawParameterValue (id); };

    rawParams.mix        = bind (ids::mix);
    rawParams.loopLength = bind (ids::loopLength);
    rawParams.loopStart  = bind (ids::loopStart);
    rawParams.loopEnd    = bind (ids::loopEnd);
    rawParams.speed      = bind (ids::speed);
    rawParams.direction  = bind (ids::direction);
    rawParams.feedback   = bind (ids::feedback);
    rawParams.attack     = bind (ids::attack);
    rawParams.decay      = bind (ids::decay);
    rawParams.sustain    = bind (ids::sustain);
    rawParams.release    = bind (ids::release);
    rawParams.wear       = bind (ids::wear);
    rawParams.dropouts   = bind (ids::dropouts);
    rawParams.splice     = bind (ids::splice);
    rawParams.oxide      = bind (ids::oxide);
    rawParams.wow        = bind (ids::wow);
    rawParams.hiss       = bind (ids::hiss);
    rawParams.tone       = bind (ids::tone);
    rawParams.sat        = bind (ids::sat);
    rawParams.output     = bind (ids::output);
    rawParams.limiter    = bind (ids::limiter);
    rawParams.transport  = bind (ids::transport);
    rawParams.syncTransport = bind (ids::syncTransport);
}

//==============================================================================
APVTS::ParameterLayout SomniaAudioProcessor::createParameterLayout()
{
    using namespace somnia;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto addFloat = [&params] (const char* id, const juce::String& name,
                               float min, float max, float def,
                               const juce::String& suffix = {})
    {
        auto attrs = juce::AudioParameterFloatAttributes()
                         .withLabel (suffix)
                         .withStringFromValueFunction ([] (float v, int)
                         {
                             auto s = juce::String (v, 4);
                             if (s.containsChar ('.'))
                             {
                                 s = s.trimCharactersAtEnd ("0");
                                 s = s.trimCharactersAtEnd (".");
                             }
                             return s;
                         });

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            id, name, juce::NormalisableRange<float> (min, max), def, attrs));
    };

    auto addChoice = [&params] (const char* id, const juce::String& name,
                                const juce::StringArray& choices, int def)
    {
        params.push_back (std::make_unique<juce::AudioParameterChoice> (id, name, choices, def));
    };

    // --- global --------------------------------------------------------------
    addFloat (ids::mix, "Mix", 0.0f, 1.0f, 0.70f);

    // --- tape transport ------------------------------------------------------
    addChoice (ids::loopLength, "Loop Length",
               { "Free", "1/1", "2/1", "4/1", "8/1", "16/1" }, 3);
    addFloat (ids::loopStart, "Loop Start", 0.0f, 1.0f, 0.0f);
    addFloat (ids::loopEnd, "Loop End", 0.0f, 1.0f, 1.0f);
    addFloat (ids::speed, "Speed", -24.0f, 24.0f, 0.0f, "st");
    addChoice (ids::direction, "Direction",
               { "Forward", "Reverse", "Ping-Pong", "Random" }, 0);
    addFloat (ids::feedback, "Feedback", 0.0f, 0.95f, 0.55f);
    addChoice (ids::transport, "Transport", { "Stop", "Play", "Rec" }, 0);
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        ids::syncTransport, "Sync Transport", true));

    // --- loop envelope -------------------------------------------------------
    addFloat (ids::attack, "Attack", 1.0f, 2000.0f, 20.0f, "ms");
    addFloat (ids::decay, "Decay", 1.0f, 2000.0f, 200.0f, "ms");
    addFloat (ids::sustain, "Sustain", 0.0f, 1.0f, 0.90f);
    addFloat (ids::release, "Release", 1.0f, 4000.0f, 150.0f, "ms");

    // --- disintegration ------------------------------------------------------
    addFloat (ids::wear, "Wear", 0.0f, 1.0f, 0.35f);
    addFloat (ids::dropouts, "Dropouts", 0.0f, 1.0f, 0.25f);
    addFloat (ids::splice, "Splice", 0.0f, 1.0f, 0.10f);
    addFloat (ids::oxide, "Oxide", 0.0f, 1.0f, 0.20f);
    addFloat (ids::wow, "Wow", 0.0f, 1.0f, 0.20f);
    addFloat (ids::hiss, "Hiss", 0.0f, 1.0f, 0.12f);

    // --- tone / output -------------------------------------------------------
    addFloat (ids::tone, "Tone", 0.0f, 1.0f, 0.70f);
    addFloat (ids::sat, "Sat", 0.0f, 1.0f, 0.20f);
    addFloat (ids::output, "Output", -24.0f, 12.0f, 0.0f, "dB");
    params.push_back (std::make_unique<juce::AudioParameterBool> (ids::limiter, "Limiter", true));

    return { params.begin(), params.end() };
}

//==============================================================================
void SomniaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    setLatencySamples (0);

    // The scratch buffers are sized here, once: setSize() in processBlock would
    // allocate on the audio thread.  samplesPerBlock is the host's upper bound,
    // and a little headroom keeps a host that rounds its block size up happy.
    bufferCapacity = juce::jmax (samplesPerBlock, 512) + 256;

    tapeLoop.prepare (sampleRate, 60.0);
    tapeDegrader.prepare (sampleRate);
    loopAdsr.prepare (sampleRate);
    outputLimiter.prepare (sampleRate);

    loopBuffer.setSize (2, bufferCapacity, false, false, true);
    wetBuffer.setSize (2, bufferCapacity, false, false, true);
    outBuffer.setSize (2, bufferCapacity, false, false, true);

    mixSmoothed.reset (sampleRate, 0.02);
    outputSmoothed.reset (sampleRate, 0.02);
    mixSmoothed.setCurrentAndTargetValue (rawParams.mix->load());
    outputSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (rawParams.output->load()));

    speedSmoothed.reset (sampleRate, 0.06);
    speedSmoothed.setCurrentAndTargetValue (
        std::pow (2.0f, rawParams.speed->load() / 12.0f));

    lastTransport = -1;
    effectiveTransport = 0;
    lastParamTransport = -1;
    pendingTransport.store (-1, std::memory_order_relaxed);
    lastHostPlaying = false;
    hostTransportSeen = false;
    logTimeSec = 0.0;

    debugLogger.start();
}

void SomniaAudioProcessor::releaseResources()
{
    debugLogger.stop();
}

bool SomniaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

//==============================================================================
void SomniaAudioProcessor::applyTransport (int mode)
{
    if (mode == 0)
    {
        tapeLoop.stopRecording();
        tapeLoop.stopPlayback();
        loopAdsr.noteOff();
    }
    else if (mode == 1)
    {
        tapeLoop.stopRecording();
        if (! tapeLoop.isPlaying())
        {
            tapeLoop.startPlayback();
            loopAdsr.noteOn();
        }
    }
    else
    {
        if (! tapeLoop.isRecording())
        {
            const bool overdub = tapeLoop.hasContent();
            const bool wasPlaying = tapeLoop.isPlaying();
            tapeLoop.startRecording (overdub);

            // Punching in over a running loop must not restart the envelope:
            // the wet signal would fade from zero and the take would open with
            // an audible dip.
            if (! wasPlaying)
                loopAdsr.noteOn();
        }
    }
}

void SomniaAudioProcessor::updateLoopLength()
{
    const int choice = (int) rawParams.loopLength->load();

    if (choice <= 0)
    {
        tapeLoop.setFreeMode (true);
        return;
    }

    tapeLoop.setFreeMode (false);
    const int idx = juce::jlimit (0, 4, choice - 1);
    // kLoopBeats holds whole notes; a whole note is four beats in 4/4.
    const double seconds = somnia::kLoopBeats[idx] * 4.0 * 60.0 / currentBpm;
    tapeLoop.setTapeLengthSeconds (seconds);
}

void SomniaAudioProcessor::applyLoopBounds()
{
    const double tapeSeconds = tapeLoop.getTapeLengthSeconds();
    if (tapeSeconds <= 0.0)
        return;

    const double start = (double) rawParams.loopStart->load() * tapeSeconds;
    const double end   = (double) rawParams.loopEnd->load() * tapeSeconds;
    tapeLoop.setLoopBoundsSeconds (start, end);
}

void SomniaAudioProcessor::resetDsp()
{
    tapeDegrader.reset();
    loopAdsr.reset();
    outputLimiter.reset();

    // A non-finite block means something upstream produced garbage; leaving the
    // tape running would just feed the broken state back into the loop.  The
    // transport is re-gated so the next block re-applies it from scratch instead
    // of believing the tape is already in the requested state.
    tapeLoop.stopPlayback();
    tapeLoop.stopRecording();
    lastTransport = -1;
}

//==============================================================================
void SomniaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();

    if (numSamples == 0)
        return;

    // --- tempo ---------------------------------------------------------------
    bool hostPlaying = false;
    bool hostHasTransport = false;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            hostHasTransport = true;

            if (auto bpm = pos->getBpm())
                currentBpm = juce::jlimit (20.0, 300.0, *bpm);

            hostPlaying = pos->getIsPlaying();
        }
    }

    displayBpm.store ((float) currentBpm, std::memory_order_relaxed);

    // The loop length has to be known before a synced recording starts, so the
    // first REC already writes the musical length instead of a fallback.
    updateLoopLength();
    applyLoopBounds();

    // --- transport -----------------------------------------------------------
    // With Sync Transport on, a host *transition* drives the tape: rolling the
    // DAW starts playback, stopping it stops playback and closes a recording.
    // Only transitions are followed, so REC and STOP remain usable while the
    // DAW sits still (a steady "host is stopped" state must not cancel a manual
    // REC on every block).  The parameter is never rewritten from the audio
    // thread, so the resulting state is latched in effectiveTransport; the UI
    // reads the real state from telemetry.
    const int paramTransport = (int) rawParams.transport->load();
    int transport = effectiveTransport;

    // UI buttons go through setTransport(), which also leaves a request here:
    // pressing REC again after the host forced the tape to stop must work even
    // though the parameter value itself did not change.
    if (const int requested = pendingTransport.exchange (-1, std::memory_order_relaxed);
        requested >= 0)
        transport = requested;
    else if (paramTransport != lastParamTransport)
        transport = paramTransport;   // automation, preset or state change

    lastParamTransport = paramTransport;

    const bool hostStarted = hostHasTransport && hostPlaying && ! lastHostPlaying;
    const bool hostStopped = hostHasTransport && ! hostPlaying && lastHostPlaying;

    if (hostHasTransport)
    {
        lastHostPlaying = hostPlaying;
        hostTransportSeen = true;
    }

    if (rawParams.syncTransport->load() > 0.5f && hostTransportSeen)
    {
        if (hostStopped)
            transport = 0;
        else if (hostStarted && transport == 0)
            transport = 1;
    }

    effectiveTransport = transport;

    if (transport != lastTransport)
    {
        applyTransport (transport);
        lastTransport = transport;
    }

    if (clearRequested.exchange (false))
    {
        tapeLoop.clear();
        loopAdsr.reset();
        lastTransport = -1;   // re-apply the current transport next block
    }

    if (const int rw = rewindRequests.exchange (0); rw > 0)
        tapeLoop.rewind (0.35 * (double) rw);

    if (const int ff = forwardRequests.exchange (0); ff > 0)
        tapeLoop.fastForward (0.35 * (double) ff);

    // --- parameters ----------------------------------------------------------
    const float attackSec  = rawParams.attack->load() * 0.001f;
    const float decaySec   = rawParams.decay->load() * 0.001f;
    const float releaseSec = rawParams.release->load() * 0.001f;

    loopAdsr.setParams (attackSec, decaySec,
                        rawParams.sustain->load(),
                        releaseSec);

    // Retriggering the envelope on every pass only makes sense when the pass is
    // long enough for the envelope to complete.  On a short loop the attack
    // would restart before reaching sustain, which made the loop nearly silent
    // — the "PLAY stops working" report.  A pass at 2x speed takes half the
    // wall-clock time, so the varispeed has to be part of the comparison.
    const double envCycleSec = (double) attackSec + (double) decaySec + (double) releaseSec;
    const double passSeconds = tapeLoop.getLoopLengthSeconds()
                               / juce::jmax (0.25, (double) speedSmoothed.getCurrentValue());
    const bool retriggerOnWrap = passSeconds > envCycleSec * 1.15;

    somnia::TapeDegrader::Params deg;
    deg.wear     = rawParams.wear->load();
    deg.dropouts = rawParams.dropouts->load();
    deg.oxide    = rawParams.oxide->load();
    deg.wow      = rawParams.wow->load();
    deg.hiss     = rawParams.hiss->load();
    deg.tone     = rawParams.tone->load();
    deg.sat      = rawParams.sat->load();

    somnia::TapeLoop::Params tp;
    speedSmoothed.setTargetValue (std::pow (2.0f, rawParams.speed->load() / 12.0f));
    tp.speed     = speedSmoothed.getCurrentValue();   // re-ramped per chunk below
    tp.feedback  = rawParams.feedback->load();
    tp.direction = (int) rawParams.direction->load();
    tp.playing   = tapeLoop.isPlaying() || loopAdsr.isActive();
    tp.splice    = rawParams.splice->load();

    // Musical grid for splices: one bar from the host tempo, shortened to half
    // the loop when the loop is shorter than a bar.
    {
        const double barSeconds = 4.0 * 60.0 / currentBpm;
        const double loopSeconds = tapeLoop.getLoopLengthSeconds();
        tp.phraseSeconds = (float) (loopSeconds > 0.0
                                        ? juce::jmin (barSeconds, loopSeconds * 0.5)
                                        : barSeconds);
    }

    // Dropouts land on phrase boundaries too, so gaps feel like musical rests
    // instead of random glitches.
    {
        const double loopSeconds = tapeLoop.getLoopLengthSeconds();
        if (loopSeconds > 0.0 && tp.phraseSeconds > 0.0f)
        {
            const double phraseFraction = juce::jlimit (0.001, 1.0,
                                                        (double) tp.phraseSeconds / loopSeconds);
            deg.phrasePhase = (float) std::fmod (tapeLoop.getPositionNormalised()
                                                     / phraseFraction,
                                                 1.0);
        }
    }

    // --- buffers -------------------------------------------------------------
    // The scratch buffers were sized in prepareToPlay().  Growing them here is
    // only a last-resort path for a host that ignores the samplesPerBlock it
    // promised; it must never run in steady state, because setSize() allocates.
    if (numSamples > bufferCapacity)
    {
        bufferCapacity = numSamples;
        loopBuffer.setSize (2, bufferCapacity, false, false, true);
        wetBuffer.setSize (2, bufferCapacity, false, false, true);
        outBuffer.setSize (2, bufferCapacity, false, false, true);
    }

    // The host input is kept untouched until the final write-back, so the dry
    // path can read it directly.  A separate dry copy used to cost a full
    // block-wide sample-by-sample copy every processBlock.
    const float* inL = buffer.getReadPointer (0);
    const float* inR = numCh > 1 ? buffer.getReadPointer (1) : inL;

    float* loopL = loopBuffer.getWritePointer (0);
    float* loopR = loopBuffer.getWritePointer (1);
    float* wetL  = wetBuffer.getWritePointer (0);
    float* wetR  = wetBuffer.getWritePointer (1);

    float inPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        inPeak = juce::jmax (inPeak, juce::jmax (std::abs (inL[i]), std::abs (inR[i])));

    // --- tape engine (chunked so the ADSR and the loop wrap stay responsive) --
    constexpr int chunkSize = 64;

    // Per-stage peaks are tracked inside the engine loop: an extra pass over
    // every buffer just for the black box used to read back data that had
    // already left the cache.
    float loopPeak = 0.0f, degPeak = 0.0f, wetPeak = 0.0f;

    const bool overdubMonitor = tapeLoop.isRecording()
                             && tapeLoop.hasContent()
                             && ! tapeLoop.isFreshRecording();

    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - offset);

        // The varispeed ramp is advanced per chunk instead of once per block:
        // SLOW / FAST then glides through the chunk grid (64 samples) rather than
        // stepping in whole host buffers, which was audible as a staircase pitch
        // on large block sizes.
        tp.speed = speedSmoothed.skip (n);

        tapeLoop.read (loopL + offset, loopR + offset, n, tp, rng);

        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (loopL[offset + i]);
            const float b = std::abs (loopR[offset + i]);
            loopPeak = juce::jmax (loopPeak, juce::jmax (a, b));
        }

        if (tapeLoop.consumeWrapFlag()
            && retriggerOnWrap
            && (tapeLoop.isPlaying() || tapeLoop.isRecording()))
            loopAdsr.noteOn();

        tapeDegrader.process (loopL + offset, loopR + offset, n, deg, rng);

        // The envelope only shapes the audible wet signal; the degraded loop
        // (without the envelope) is what gets written back into the tape.
        for (int i = 0; i < n; ++i)
        {
            const float dl = loopL[offset + i];
            const float dr = loopR[offset + i];

            degPeak = juce::jmax (degPeak, juce::jmax (std::abs (dl), std::abs (dr)));

            const float env = loopAdsr.process();
            const float wl = dl * env;
            const float wr = dr * env;

            wetL[offset + i] = wl;
            wetR[offset + i] = wr;

            if (overdubMonitor)
            {
                // Punch-in monitoring: the input rides the wet path at unity so
                // the direct signal keeps its level while the loop plays under it.
                const float ml = wl + inL[offset + i];
                const float mr = wr + inR[offset + i];
                wetL[offset + i] = ml;
                wetR[offset + i] = mr;
                wetPeak = juce::jmax (wetPeak, juce::jmax (std::abs (ml), std::abs (mr)));
            }
            else
            {
                wetPeak = juce::jmax (wetPeak, juce::jmax (std::abs (wl), std::abs (wr)));
            }
        }

        tapeLoop.write (inL + offset, inR + offset,
                        loopL + offset, loopR + offset, n, tp);
    }

    // A free take that filled the whole tape stopped itself inside write().
    // The transport parameter still says "Rec" and a same-value write would not
    // be observable, so fall back to playback here: the take becomes audible and
    // the next press of REC punches in over it instead of doing nothing.
    if (tapeLoop.consumeTapeFull())
    {
        effectiveTransport = 1;
        lastTransport = 1;
        tapeLoop.startPlayback();
        loopAdsr.noteOn();
    }

    // --- dry / wet + output --------------------------------------------------
    // An empty or stopped tape produces no wet signal.  Applying the dry/wet
    // mix in that state would attenuate the input (at the default mix of 0.7
    // the dry path loses 10 dB), which is very audible when Somnia sits in a
    // chain after another instrument.  A fresh recording is the same case: the
    // wet path is the input being written, so mixing it in only ducks the dry.
    const bool wetActive = tapeLoop.hasContent()
                        && ! tapeLoop.isFreshRecording()
                        && (tapeLoop.isPlaying() || tapeLoop.isRecording()
                            || loopAdsr.isActive());

    // During a punch-in the wet path already carries the dry input, so the
    // mix is driven to 1.0: applying the user mix on top would double-count
    // the dry signal and duck the direct input.
    mixSmoothed.setTargetValue (overdubMonitor ? 1.0f
                             : (wetActive ? rawParams.mix->load() : 0.0f));
    outputSmoothed.setTargetValue (
        juce::Decibels::decibelsToGain (rawParams.output->load()));

    float* outL = outBuffer.getWritePointer (0);
    float* outR = outBuffer.getWritePointer (1);
    float mixPeak = 0.0f;

    if (overdubMonitor)
    {
        // The wet path is already input + loop at unity; skip the (1 - m)
        // crossfade and just apply the output gain.
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputSmoothed.getNextValue();
            mixSmoothed.skip (1);
            const float l = wetL[i] * g;
            const float r = wetR[i] * g;
            mixPeak = juce::jmax (mixPeak, juce::jmax (std::abs (l), std::abs (r)));
            outL[i] = l;
            outR[i] = r;
        }
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float m = mixSmoothed.getNextValue();
            const float g = outputSmoothed.getNextValue();
            const float l = (inL[i] * (1.0f - m) + wetL[i] * m) * g;
            const float r = (inR[i] * (1.0f - m) + wetR[i] * m) * g;
            mixPeak = juce::jmax (mixPeak, juce::jmax (std::abs (l), std::abs (r)));
            outL[i] = l;
            outR[i] = r;
        }
    }

    // --- limiter -------------------------------------------------------------
    const bool limiterOn = rawParams.limiter->load() > 0.5f;
    if (limiterOn)
        outputLimiter.process (outL, outR, numSamples, 0.98f);

    // --- final safety --------------------------------------------------------
    bool  nonFinite = false;
    float outPeak   = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        if (! std::isfinite (outL[i]) || ! std::isfinite (outR[i]))
        {
            nonFinite = true;
            outL[i] = 0.0f;
            outR[i] = 0.0f;
            continue;
        }

        // Measured before the safety clamp: afterwards the output can never
        // exceed 1.0, so a threshold above it could never be reached and the
        // OUT_HOT flag was dead.
        outPeak = juce::jmax (outPeak, juce::jmax (std::abs (outL[i]),
                                                  std::abs (outR[i])));

        outL[i] = juce::jlimit (-1.0f, 1.0f, outL[i]);
        outR[i] = juce::jlimit (-1.0f, 1.0f, outR[i]);
    }

    if (nonFinite)
        resetDsp();

    // --- telemetry -----------------------------------------------------------
    displayLoopPos.store ((float) tapeLoop.getPositionNormalised(), std::memory_order_relaxed);
    displayLoopLen.store ((float) tapeLoop.getLoopLengthSeconds(), std::memory_order_relaxed);
    displayTapeLen.store ((float) tapeLoop.getTapeLengthSeconds(), std::memory_order_relaxed);
    displayLoopStart.store ((float) tapeLoop.getLoopStartSeconds(), std::memory_order_relaxed);
    displayLoopEnd.store ((float) tapeLoop.getLoopEndSeconds(), std::memory_order_relaxed);
    displayHasContent.store (tapeLoop.hasContent() ? 1.0f : 0.0f,
                             std::memory_order_relaxed);
    displayFreshRecording.store (tapeLoop.isFreshRecording() ? 1.0f : 0.0f,
                                 std::memory_order_relaxed);
    displayWear.store (deg.wear, std::memory_order_relaxed);
    // A sample-and-hold keeps the output gain at unity (the effect is the
    // freeze, not a gap), so the UI has to be told about the hold explicitly.
    displayDropout.store (tapeDegrader.isHolding() ? 0.0f
                                                  : tapeDegrader.getDropoutGain(),
                          std::memory_order_relaxed);
    displayCycles.store (tapeLoop.getCycleCount(), std::memory_order_relaxed);
    displayDirection.store (tapeLoop.getDirectionSign(), std::memory_order_relaxed);
    displayRecording.store (tapeLoop.isRecording() ? 1.0f : 0.0f, std::memory_order_relaxed);
    displayPlaying.store (tapeLoop.isPlaying() ? 1.0f : 0.0f, std::memory_order_relaxed);
    displayEnvLevel.store (loopAdsr.getLevel(), std::memory_order_relaxed);
    displayEnvStage.store (loopAdsr.getStageIndex(), std::memory_order_relaxed);
    displayGrDb.store (limiterOn ? outputLimiter.getGainReductionDb() : 0.0f,
                       std::memory_order_relaxed);
    displayLimiting.store (limiterOn && outputLimiter.isLimiting() ? 1.0f : 0.0f,
                           std::memory_order_relaxed);

    // --- black box -----------------------------------------------------------
    // The peaks come from the engine / mix loops above, so the log reflects the
    // actual stages: loopPeak is the raw read head, degPeak the degraded signal
    // that is written back, mixPeak the pre-limiter sum and outPeak the final
    // safety-clamped result.  Previously degPeak was a copy of loopPeak and
    // mixPeak == outPeak, which hid exactly the runaway-feedback case the
    // black box exists for.
    somnia::BlockSnapshot snapshot;
    snapshot.timeSec   = logTimeSec;
    snapshot.inPeak    = inPeak;
    snapshot.loopPeak  = loopPeak;
    snapshot.degPeak   = degPeak;
    snapshot.wetPeak   = wetPeak;
    snapshot.mixPeak   = mixPeak;
    snapshot.outPeak   = outPeak;
    snapshot.grDb      = displayGrDb.load (std::memory_order_relaxed);
    snapshot.mix       = rawParams.mix->load();
    snapshot.feedback  = rawParams.feedback->load();
    snapshot.wear      = deg.wear;
    snapshot.dropouts  = deg.dropouts;
    snapshot.splice    = tp.splice;
    snapshot.oxide     = deg.oxide;
    snapshot.wow       = deg.wow;
    snapshot.hiss      = deg.hiss;
    snapshot.tone      = deg.tone;
    snapshot.sat       = deg.sat;
    snapshot.speed     = rawParams.speed->load();
    snapshot.output    = rawParams.output->load();
    snapshot.direction = tp.direction;
    snapshot.loopChoice = (int) rawParams.loopLength->load();
    snapshot.cycles    = tapeLoop.getCycleCount();
    snapshot.recording = tapeLoop.isRecording();
    snapshot.playing   = tapeLoop.isPlaying();
    snapshot.freeMode  = tapeLoop.isFreeMode();
    snapshot.limiterOn = limiterOn;

    if (nonFinite)                              snapshot.flags |= somnia::kFlagNonFinite;
    if (snapshot.inPeak > 2.0f)                 snapshot.flags |= somnia::kFlagInputHot;

    // The read head can never exceed the tape's softClip ceiling (1.5), so the
    // runaway-loop check has to look at what is written into the tape *before*
    // clipping — that is the only place a feedback loop running away is visible.
    const float tapeWritePeak = tapeLoop.consumeWritePeak();
    if (tapeWritePeak > 4.0f)                   snapshot.flags |= somnia::kFlagLoopHot;

    if (snapshot.outPeak > 1.5f)                snapshot.flags |= somnia::kFlagOutHot;
    if (snapshot.grDb > 24.0f)                  snapshot.flags |= somnia::kFlagHeavyGR;

    debugLogger.push (snapshot);
    logTimeSec += (double) numSamples / currentSampleRate;

    // --- host buffer ---------------------------------------------------------
    if (numCh >= 2)
    {
        buffer.copyFrom (0, 0, outL, numSamples);
        buffer.copyFrom (1, 0, outR, numSamples);
    }
    else
    {
        float* d = buffer.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            d[i] = 0.5f * (outL[i] + outR[i]);
    }
}

//==============================================================================
void SomniaAudioProcessor::setTransport (int mode)
{
    const int m = juce::jlimit (0, 2, mode);

    // Latched for the audio thread: the parameter value may already be equal to
    // m (for example REC is still stored while the host stopped the tape), and
    // a same-value parameter write produces no observable change.
    pendingTransport.store (m, std::memory_order_relaxed);

    if (auto* p = apvts.getParameter (somnia::ids::transport))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) m));
}

void SomniaAudioProcessor::clearLoop()
{
    clearRequested.store (true, std::memory_order_relaxed);
}

void SomniaAudioProcessor::rewindLoop()
{
    rewindRequests.fetch_add (1, std::memory_order_relaxed);
}

void SomniaAudioProcessor::fastForwardLoop()
{
    forwardRequests.fetch_add (1, std::memory_order_relaxed);
}

void SomniaAudioProcessor::nudgeSpeed (float semitones)
{
    auto* p = apvts.getParameter (somnia::ids::speed);
    if (p == nullptr)
        return;

    const auto range = p->getNormalisableRange();
    const float current = p->convertFrom0to1 (p->getValue());

    // Snap to whole semitones so repeated presses walk a musical scale instead
    // of accumulating whatever fraction the knob or RANDOM left behind.
    const float target = juce::jlimit (range.start, range.end,
                                       std::round (current) + semitones);

    p->setValueNotifyingHost (p->convertTo0to1 (target));
}

//==============================================================================
void SomniaAudioProcessor::randomizeParameters()
{
    juce::Random localRng;

    auto setValue = [this] (const char* id, float value)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    };

    auto rnd = [&localRng] (float a, float b)
    {
        return a + localRng.nextFloat() * (b - a);
    };

    // A symmetric jitter around a correlated centre.
    auto jit = [&] (float centre, float spread, float lo, float hi)
    {
        return juce::jlimit (lo, hi, centre + rnd (-spread, spread));
    };

    // Somnia's random is built from three musical axes rather than fifteen
    // independent rolls.  Every knob stays correlated with its axis, so a press
    // lands on a *character* (a dark slow decay, a lively splice garden) instead
    // of noise, while the per-knob jitter keeps the flavour of chance.
    //
    //   decay    how far the tape has come apart: level, top end, dropouts, hiss
    //   motion   time instability: wow, splices, adventurous directions
    //   pace     the feel of a pass: varispeed and envelope scale
    const float decayAxis  = std::pow (localRng.nextFloat(), 0.85f);
    const float motionAxis = localRng.nextFloat();
    const float paceAxis   = localRng.nextFloat();

    // --- the loop itself ------------------------------------------------------
    // The loop length belongs to the user: it decides how much material a take
    // has to hold, and a press of RANDOM changing it threw away a take the user
    // had already set up.  The current choice is read back and used only to scale
    // the envelope to the pass the take will actually get.
    const int loopChoice = juce::jlimit (0, 5, (int) std::round (rawParams.loopLength->load()));

    // Tempo from the host (falling back to the stored display value) so the
    // envelope can be scaled to the loop the take will actually get.
    const double bpm = juce::jlimit (20.0, 300.0,
                                     getDisplayBpm() > 0.0f ? (double) getDisplayBpm()
                                                            : (double) currentBpm);
    const double loopSeconds = loopChoice > 0
                                   ? somnia::kLoopBeats[juce::jlimit (0, 4, loopChoice - 1)]
                                         * 4.0 * 60.0 / bpm
                                   : 4.0 * 60.0 / bpm * 2.0;   // Free: about two bars

    // --- varispeed ------------------------------------------------------------
    // A heavily worn tape reads better slower and darker; a fresh one can afford
    // to be pushed up.  The centre is the decay axis and the jitter stays small
    // enough that a worn take never falls below -6 st, which is where a low tone
    // and a slow tape stop agreeing and the material turns to mud.
    const float pitchCentre = -decayAxis * 4.5f + (paceAxis - 0.5f) * 2.5f;
    setValue (somnia::ids::speed, jit (pitchCentre, 1.2f, -7.0f, 5.0f));

    // --- direction ------------------------------------------------------------
    // Motion decides how adventurous the head is; ping-pong and random are the
    // reward for a high-motion take.
    const int dirRoll = (int) (motionAxis * 3.999f);
    const int direction = motionAxis < 0.35f ? (localRng.nextFloat() < 0.6f ? 0 : 1)
                          : motionAxis < 0.7f ? (dirRoll % 2 == 0 ? 0 : 2)
                                              : localRng.nextInt (2) == 0 ? 2 : 3;
    setValue (somnia::ids::direction, (float) direction);

    // Ping-pong crosses the window twice per pass, so a long envelope has to be
    // shortened or the loop never reaches sustain.
    const double envDivisor = direction == 2 ? 2.0 : 1.0;

    // --- envelope scaled to the loop -----------------------------------------
    // The whole envelope budget is a fraction of one pass: the loop always has
    // room to breathe, which is what made the previous random mute short loops.
    const double envBudget = loopSeconds / envDivisor * (0.10 + 0.28 * paceAxis);
    const double attackFrac  = 0.10 + 0.18 * localRng.nextFloat();
    const double decayFrac   = 0.40 + 0.30 * localRng.nextFloat();
    const double releaseFrac = 1.0 - attackFrac - decayFrac;

    setValue (somnia::ids::attack,
              jit ((float) (attackFrac * envBudget * 1000.0), 8.0f, 1.0f, 2000.0f));
    setValue (somnia::ids::decay,
              jit ((float) (decayFrac * envBudget * 1000.0), 25.0f, 1.0f, 2000.0f));
    setValue (somnia::ids::release,
              jit ((float) (releaseFrac * envBudget * 1000.0), 25.0f, 1.0f, 4000.0f));

    // A long, slow decay reads as a fading tape; a short one as a pulse.
    setValue (somnia::ids::sustain,
              jit (0.92f - 0.30f * (float) decayFrac, 0.06f, 0.55f, 1.0f));

    // --- disintegration -------------------------------------------------------
    // Everything on the decay axis moves together, so a worn tape is dark, quiet
    // and dropping out *at the same time* rather than by accident.
    setValue (somnia::ids::wear,     jit (0.15f + 0.70f * decayAxis, 0.07f, 0.0f, 0.95f));
    setValue (somnia::ids::dropouts, jit (0.05f + 0.55f * decayAxis, 0.08f, 0.0f, 0.75f));
    setValue (somnia::ids::oxide,    jit (0.05f + 0.70f * decayAxis, 0.10f, 0.0f, 0.90f));
    setValue (somnia::ids::hiss,     jit (0.02f + 0.30f * decayAxis, 0.05f, 0.0f, 0.45f));
    setValue (somnia::ids::tone,     jit (0.90f - 0.55f * decayAxis, 0.06f, 0.20f, 1.0f));
    setValue (somnia::ids::sat,      jit (0.05f + 0.40f * decayAxis, 0.08f, 0.0f, 0.65f));

    // Splice and wow ride the motion axis; a very short window gets fewer
    // splices because there are simply fewer phrase boundaries to jump between.
    const double phrases = loopSeconds > 0.0
                               ? juce::jmax (1.0, juce::jmin (8.0, loopSeconds / (4.0 * 60.0 / bpm)))
                               : 1.0;
    const float spliceScale = (float) juce::jlimit (0.25, 1.0, phrases / 4.0);

    setValue (somnia::ids::splice, jit (0.02f + 0.55f * motionAxis * spliceScale,
                                        0.06f, 0.0f, 0.75f));
    setValue (somnia::ids::wow,    jit (0.03f + 0.55f * motionAxis, 0.07f, 0.0f, 0.75f));

    // Feedback closes the loop: a heavily decayed tape needs less write-back per
    // pass or the generations collapse into mush within seconds.
    setValue (somnia::ids::feedback,
              jit (0.82f - 0.30f * decayAxis - 0.10f * motionAxis, 0.05f, 0.35f, 0.90f));

    // mix, output, transport, limiter, the loop length and the loop window are
    // deliberately left alone: the user's level, playing state and the take
    // they set up must survive a press of RANDOM.
}

//==============================================================================
void SomniaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void SomniaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
        stateRestored.store (true, std::memory_order_relaxed);
    }
}

//==============================================================================
juce::AudioProcessorEditor* SomniaAudioProcessor::createEditor()
{
    return new SomniaAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SomniaAudioProcessor();
}
