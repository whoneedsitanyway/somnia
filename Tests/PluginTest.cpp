// Integration test for the real Somnia processor: defaults, free recording,
// playback, BPM-synced loop, randomisation guard and state round-trip.  Also
// renders the editor once to catch UI/layout crashes.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <iostream>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/PresetManager.h"

namespace
{
    constexpr double sr = 48000.0;
    constexpr int block = 512;

    // Runs the message loop for a while so component timers actually fire.
    // MessageManager::runDispatchLoop() is useless here: on macOS it calls
    // [NSApp run], and a console test has no NSApplication, so it returns at
    // once.  Pumping the CFRunLoop directly dispatches both JUCE timers and
    // posted messages.  Only used for the editor render below.
    void pumpMessages (int ms)
    {
       #if JUCE_MAC
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;

        while (juce::Time::getMillisecondCounter() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
       #else
        juce::Thread::sleep (ms);
       #endif
    }

    // Minimal host transport stub for the Sync Transport test.
    class MockPlayHead : public juce::AudioPlayHead
    {
    public:
        bool playing = false;
        double bpm = 120.0;
        mutable int calls = 0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            ++calls;
            PositionInfo info;
            info.setBpm (bpm);
            info.setIsPlaying (playing);
            info.setTimeSignature (TimeSignature { 4, 4 });
            return info;
        }
    };

    bool renderSeconds (SomniaAudioProcessor& proc, double seconds, double& peakOut,
                        double toneHz = 220.0)
    {
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;

        const int blocks = (int) std::ceil (seconds * sr / (double) block);
        double phase = 0.0;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const float s = (float) (0.4 * std::sin (phase + 2.0 * juce::MathConstants<double>::pi
                                                                  * toneHz * (double) i / sr));
                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
            }
            phase += 2.0 * juce::MathConstants<double>::pi * toneHz * block / sr;

            proc.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                {
                    const float x = buffer.getSample (ch, i);
                    if (! std::isfinite (x) || std::abs (x) > 1.0f)
                        return false;
                    peakOut = juce::jmax (peakOut, (double) std::abs (x));
                }
        }

        return true;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    SomniaAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);

    auto get = [&] (const char* id)
    {
        return proc.apvts.getRawParameterValue (id)->load();
    };

    const bool defaultsOk = std::abs (get ("mix") - 0.70f) < 0.001f
                         && std::abs (get ("loopLength") - 2.0f) < 0.01f
                         && std::abs (get ("loopStart") - 0.0f) < 0.001f
                         && std::abs (get ("loopEnd") - 1.0f) < 0.001f
                         && std::abs (get ("transport") - 0.0f) < 0.01f
                         && std::abs (get ("syncTransport") - 1.0f) < 0.01f
                         && std::abs (get ("limiter") - 1.0f) < 0.01f;

    // --- bypass on an empty tape --------------------------------------------
    // With no recorded material there is no wet signal, so the dry input must
    // pass through at full level.  Applying the dry/wet mix here used to cost
    // 10 dB at the default mix of 0.7, which was very audible in a chain.
    {
        double dryPeak = 0.0;
        const bool dryFinite = renderSeconds (proc, 0.25, dryPeak);
        const bool dryOk = dryFinite && dryPeak > 0.35 && dryPeak < 0.45;

        if (! dryOk)
        {
            std::cout << "empty-tape dry peak=" << dryPeak << std::endl;
            return 1;
        }
    }

    // --- free recording ------------------------------------------------------
    if (auto* p = proc.apvts.getParameter ("loopLength"))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // Free

    proc.setTransport (2);
    double peak = 0.0;
    bool finite = renderSeconds (proc, 1.2, peak);
    proc.setTransport (0);
    finite = renderSeconds (proc, 0.05, peak) && finite;

    // Recording a fresh take must not duck the input: the wet path is just the
    // material being written, so the dry signal has to stay at full level.
    const bool recLevelOk = peak > 0.35 && peak < 0.55;

    const float freeLen = proc.getDisplayLoopLengthSeconds();
    const bool freeOk = freeLen > 1.0f && freeLen < 1.4f
                     && ! proc.getDisplayRecording();

    // --- playback ------------------------------------------------------------
    proc.setTransport (1);
    finite = renderSeconds (proc, 2.0, peak) && finite;

    const bool playOk = proc.getDisplayPlaying()
                     && proc.getDisplayCycles() > 0
                     && peak > 0.01;

    // --- overdub must not duck the input ------------------------------------
    // Punching in over an existing loop used to attenuate the direct signal by
    // the dry/wet mix (0.3x = -10.5 dB at the default).  The input is monitored
    // on the wet path now, so its level stays at unity.
    bool overdubLevelOk = true;
    {
        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        // The input alone is 0.4, so the check needs the loop to contribute.
        // With the default disintegration a dropout or a wow trough could land
        // on the measured window and push the sum below the threshold, which
        // made this test flaky rather than wrong.  Silencing the degraders and
        // holding the envelope open makes the level check deterministic.
        static constexpr const char* kClean[] = { "wear", "dropouts", "oxide",
                                                  "wow", "hiss" };
        float saved[5] = {};
        int i = 0;
        for (const char* id : kClean)
            saved[i++] = get (id);

        for (const char* id : kClean)
            setP (id, 0.0f);

        const float savedAttack  = get ("attack");
        const float savedDecay   = get ("decay");
        const float savedSustain = get ("sustain");
        setP ("attack", 0.0f);
        setP ("decay", 0.0f);
        setP ("sustain", 1.0f);

        setP ("mix", 0.90f);
        proc.setTransport (1);

        double playPeak = 0.0;
        renderSeconds (proc, 0.4, playPeak);

        proc.setTransport (2);
        double recPeak = 0.0;
        // The loop holds the 220 Hz tone used for the take, so punching in with
        // the same tone would make the check depend on the phase between the dry
        // input and the loop: at an unlucky loop length they cancel and the sum
        // drops below the input alone.  A different frequency makes the two
        // components incoherent, so their peaks add up as intended.
        renderSeconds (proc, 0.6, recPeak, 1000.0);
        proc.setTransport (0);
        renderSeconds (proc, 0.05, recPeak);

        overdubLevelOk = recPeak > 0.45;

        if (! overdubLevelOk)
            std::cout << "overdub peak=" << recPeak
                      << " playPeak=" << playPeak
                      << " rec=" << proc.getDisplayRecording()
                      << " fresh=" << proc.getDisplayFreshRecording()
                      << " content=" << proc.getDisplayHasContent()
                      << " env=" << proc.getDisplayEnvelopeLevel()
                      << std::endl;

        i = 0;
        for (const char* id : kClean)
            setP (id, saved[i++]);

        setP ("attack", savedAttack);
        setP ("decay", savedDecay);
        setP ("sustain", savedSustain);
        setP ("mix", 0.70f);
    }

    // --- MIX is a dry/wet balance, not a mute --------------------------------
    // At MIX = 0 the tape is fully replaced by the untouched input: this is the
    // "I hear nothing" report when Somnia sits on a silent track or in the
    // standalone (whose input is muted), not a gain bug.
    bool mixZeroOk = true;
    {
        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        setP ("mix", 0.0f);
        proc.setTransport (1);

        double zeroPeak = 0.0;
        renderSeconds (proc, 0.5, zeroPeak);

        mixZeroOk = zeroPeak > 0.35 && zeroPeak < 0.45;

        if (! mixZeroOk)
            std::cout << "mix=0 peak=" << zeroPeak << std::endl;

        proc.setTransport (0);
        renderSeconds (proc, 0.05, zeroPeak);
        setP ("mix", 0.70f);
    }

    // --- ping-pong playback must stay audible --------------------------------
    // The direction parameter used to make PLAY go silent in some situations;
    // this checks the whole processor, not just the tape engine.
    bool pingPongOk = true;
    {
        proc.setTransport (1);

        if (auto* p = proc.apvts.getParameter ("direction"))
            p->setValueNotifyingHost (p->convertTo0to1 (2.0f));   // Ping-Pong

        double ppPeak = 0.0;
        const bool ppFinite = renderSeconds (proc, 2.0, ppPeak);
        pingPongOk = ppFinite && ppPeak > 0.02 && proc.getDisplayPlaying();

        if (! pingPongOk)
            std::cout << "ping-pong peak=" << ppPeak
                      << " playing=" << proc.getDisplayPlaying() << std::endl;

        if (auto* p = proc.apvts.getParameter ("direction"))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // back to Forward
    }

    // --- ADSR stages ---------------------------------------------------------
    // The envelope must reach sustain while playing and then run a real release
    // after STOP (it used to be inaudible because the wet path was cut at once).
    bool releaseOk = true;
    {
        proc.setTransport (1);

        double playPeak = 0.0;
        const bool playFinite = renderSeconds (proc, 0.6, playPeak);

        const int stagePlaying = proc.getDisplayEnvelopeStage();   // 3 = sustain

        proc.setTransport (0);
        renderSeconds (proc, 0.03, playPeak);
        const int stageRelease = proc.getDisplayEnvelopeStage();   // 4 = release

        renderSeconds (proc, 0.5, playPeak);
        const int stageIdle = proc.getDisplayEnvelopeStage();      // 0 = idle

        releaseOk = playFinite
                 && playPeak > 0.02
                 && stagePlaying == 3
                 && stageRelease == 4
                 && stageIdle == 0;

        if (! releaseOk)
            std::cout << "env play=" << stagePlaying << " release=" << stageRelease
                      << " idle=" << stageIdle << " peak=" << playPeak << std::endl;
    }

    // --- ping-pong must stay audible over a long run -------------------------
    {
        proc.setTransport (1);
        if (auto* p = proc.apvts.getParameter ("direction"))
            p->setValueNotifyingHost (p->convertTo0to1 (2.0f));

        double earlyPeak = 0.0;
        renderSeconds (proc, 2.0, earlyPeak);

        double latePeak = 0.0;
        renderSeconds (proc, 6.0, latePeak);

        const bool longPingOk = earlyPeak > 0.02 && latePeak > 0.02;

        if (! longPingOk)
            std::cout << "long ping-pong early=" << earlyPeak
                      << " late=" << latePeak << std::endl;

        releaseOk = releaseOk && longPingOk;

        if (auto* p = proc.apvts.getParameter ("direction"))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
    }

    // --- ping-pong across a full 8 s pass ------------------------------------
    // The default loop is 4/1 = 8 s at 120 BPM, so a short test never reaches
    // the reflection.  A head that freezes at the edge (the fractional-speed
    // bug) shows up as a dead second right after the first reflection.
    bool fullPassOk = true;
    {
        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        setP ("loopStart", 0.0f);
        setP ("loopEnd", 1.0f);
        setP ("loopLength", 3.0f);          // 4/1 -> 8 s at 120 BPM
        proc.clearLoop();
        renderSeconds (proc, 0.05, peak);

        proc.setTransport (2);
        renderSeconds (proc, 8.2, peak);
        proc.setTransport (0);
        renderSeconds (proc, 0.05, peak);

        // Half speed reaches the freeze with a fractional step.
        setP ("speed", -12.0f);
        setP ("direction", 2.0f);           // Ping-Pong
        proc.setTransport (1);

        for (int sec = 0; sec < 40; ++sec)
        {
            double secPeak = 0.0;
            renderSeconds (proc, 1.0, secPeak);

            if (secPeak < 0.05)
            {
                fullPassOk = false;
                std::cout << "pp8 t=" << (sec + 1)
                          << " peak=" << secPeak
                          << " pos=" << proc.getDisplayLoopPosition()
                          << " dir=" << proc.getDirectionSign() << std::endl;
            }
        }

        setP ("speed", 0.0f);
        setP ("direction", 0.0f);
        proc.setTransport (0);
    }

    // --- short loop + long envelope -----------------------------------------
    // The envelope used to retrigger on every pass, so a loop shorter than the
    // attack never reached sustain and sounded almost silent (twice as bad in
    // ping-pong, which wraps at both ends).
    {
        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        setP ("loopStart", 0.0f);
        setP ("loopEnd", 0.10f);          // ~120 ms window
        setP ("attack", 2000.0f);
        setP ("decay", 2000.0f);
        setP ("release", 1000.0f);
        setP ("sustain", 0.90f);

        proc.setTransport (1);
        double shortPeak = 0.0;
        const bool shortFinite = renderSeconds (proc, 5.0, shortPeak);

        const float envLevel = proc.getDisplayEnvelopeLevel();
        const bool shortEnvOk = shortFinite && envLevel > 0.80f && shortPeak > 0.01;

        if (! shortEnvOk)
            std::cout << "short loop env=" << envLevel
                      << " peak=" << shortPeak << std::endl;

        releaseOk = releaseOk && shortEnvOk;

        setP ("loopEnd", 1.0f);
        setP ("attack", 20.0f);
        setP ("decay", 200.0f);
        setP ("release", 150.0f);
        proc.setTransport (0);
    }

    // --- tempo-synced loop length -------------------------------------------
    // 1/1 is one whole note = four beats = 2 s at the default 120 BPM.
    if (auto* p = proc.apvts.getParameter ("loopLength"))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));

    proc.clearLoop();
    finite = renderSeconds (proc, 0.05, peak) && finite;   // apply the clear
    proc.setTransport (2);
    finite = renderSeconds (proc, 3.0, peak) && finite;
    proc.setTransport (0);
    finite = renderSeconds (proc, 0.05, peak) && finite;

    const float syncLen = proc.getDisplayLoopLengthSeconds();
    const bool syncOk = std::abs (syncLen - 2.0f) < 0.05f;

    // --- tape map telemetry --------------------------------------------------
    // In synced mode the tape length is fixed from the first block, so the map
    // must refresh from the write version; otherwise a fresh take stayed blank
    // while recording and the panel only filled after the length changed.
    bool waveformOk = true;
    {
        const auto v0 = proc.getDisplayWriteVersion();

        proc.setTransport (2);
        finite = renderSeconds (proc, 0.8, peak) && finite;
        proc.setTransport (0);
        finite = renderSeconds (proc, 0.05, peak) && finite;

        const auto v1 = proc.getDisplayWriteVersion();

        std::vector<float> mn ((size_t) 512, 0.0f), mx ((size_t) 512, 0.0f);
        proc.copyLoopWaveform (mn.data(), mx.data(), 512);

        float energy = 0.0f;
        for (int i = 0; i < 512; ++i)
            energy = juce::jmax (energy, mx[(size_t) i] - mn[(size_t) i]);

        waveformOk = v1 > v0 && energy > 0.01f;

        if (! waveformOk)
            std::cout << "wave version " << v0 << "->" << v1
                      << " energy=" << energy << std::endl;
    }

    // --- randomisation guard -------------------------------------------------
    const float mixBefore = get ("mix");
    const float outBefore = get ("output");
    const float transportBefore = get ("transport");
    const float loopLenBefore = get ("loopLength");

    proc.randomizeParameters();

    const bool randomOk = std::abs (get ("mix") - mixBefore) < 0.001f
                       && std::abs (get ("output") - outBefore) < 0.001f
                       && std::abs (get ("transport") - transportBefore) < 0.01f
                       && std::abs (get ("loopLength") - loopLenBefore) < 0.01f;

    // --- musicality of RANDOM ------------------------------------------------
    // RANDOM must not be fifteen independent rolls: a press has to land on a
    // playable character.  The invariants below are what makes the result sound
    // like tape music instead of noise - the envelope has to fit inside one
    // pass, the disintegration parameters have to agree with each other, and no
    // take may be driven to an extreme on every axis at once.
    bool musicalOk = true;
    {
        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        // A fresh, fully predictable state to randomize from.
        setP ("mix", 0.70f);
        setP ("output", 0.0f);
        setP ("transport", 0.0f);

        // The loop length is the user's: RANDOM must never move it, whatever it
        // currently is.  1/1 (2 s at 120 BPM) is short enough to catch an
        // envelope that does not fit inside a pass.
        setP ("loopLength", 1.0f);
        const float loopLenFixed = get ("loopLength");

        int loopLenChanged = 0;
        int extremeCount = 0;
        double minEnvFit = 1.0e9;

        for (int trial = 0; trial < 400; ++trial)
        {
            proc.randomizeParameters();

            const int   loopChoice = (int) get ("loopLength");
            const float attack  = get ("attack");
            const float decay   = get ("decay");
            const float release = get ("release");
            const float wear     = get ("wear");
            const float dropouts = get ("dropouts");
            const float oxide    = get ("oxide");
            const float hiss     = get ("hiss");
            const float tone     = get ("tone");
            const float sat      = get ("sat");
            const float splice   = get ("splice");
            const float wow      = get ("wow");
            const float feedback = get ("feedback");
            const float speed    = get ("speed");

            if (std::abs ((float) loopChoice - loopLenFixed) > 0.01f)
                ++loopLenChanged;

            // Length of one pass at the fallback tempo (no play head -> 120 BPM).
            static const double kBeats[] = { 1.0, 2.0, 4.0, 8.0, 16.0 };
            const double loopSeconds = loopChoice > 0
                                           ? kBeats[juce::jlimit (0, 4, loopChoice - 1)]
                                                 * 4.0 * 60.0 / 120.0
                                           : 4.0;

            // The envelope must complete inside a pass, otherwise the loop never
            // reaches sustain and the take is (almost) silent - the classic
            // "random made it quiet" complaint.  Ping-pong wraps twice.
            const int direction = (int) get ("direction");
            const double pass = loopSeconds / (direction == 2 ? 2.0 : 1.0);
            const double envCycle = (attack + decay + release) * 0.001;
            const double fit = envCycle / pass;
            minEnvFit = juce::jmin (minEnvFit, fit);

            if (fit > 0.60)
            {
                musicalOk = false;
                std::cout << "env too long: trial=" << trial
                          << " loop=" << loopSeconds << " dir=" << direction
                          << " env=" << envCycle << " fit=" << fit << std::endl;
            }

            // No parameter may sit on its own extreme ceiling.
            if (wear > 0.95f || dropouts > 0.78f || oxide > 0.92f || hiss > 0.46f
                || sat > 0.68f || tone < 0.14f || feedback > 0.92f || feedback < 0.33f
                || speed < -9.2f || speed > 5.2f || splice > 0.78f || wow > 0.78f)
            {
                musicalOk = false;
                std::cout << "extreme: wear=" << wear << " drop=" << dropouts
                          << " oxide=" << oxide << " hiss=" << hiss
                          << " tone=" << tone << " sat=" << sat
                          << " fb=" << feedback << " speed=" << speed << std::endl;
            }

            // The decay axis must be coherent: a dark, heavily worn tape comes
            // with a low tone and little write-back, and a fresh one must not be
            // pitch-shifted into the mud.
            const bool worn = wear > 0.70f;
            if (worn && (tone < 0.25f || feedback > 0.75f || speed < -7.5f))
            {
                musicalOk = false;
                std::cout << "incoherent decay axis: wear=" << wear
                          << " tone=" << tone << " fb=" << feedback
                          << " speed=" << speed << std::endl;
            }

            // All the disintegration knobs maxed at once is the mush case.
            if (wear > 0.85f && dropouts > 0.65f && hiss > 0.35f && sat > 0.55f)
                ++extremeCount;

            // The take has to actually be renderable.
            double takePeak = 0.0;
            if (! renderSeconds (proc, 0.10, takePeak))
            {
                musicalOk = false;
                std::cout << "non-finite after random at trial " << trial << std::endl;
            }

            setP ("transport", 0.0f);
            renderSeconds (proc, 0.05, takePeak);
        }

        // The loop length is the user's decision and RANDOM must respect it.
        if (loopLenChanged > 0)
        {
            musicalOk = false;
            std::cout << "loop length changed in " << loopLenChanged
                      << " of 400 random presses" << std::endl;
        }

        if (extremeCount > 8)
        {
            musicalOk = false;
            std::cout << "too many mush combinations=" << extremeCount << std::endl;
        }

        std::cout << "random trials: loopLenChanged=" << loopLenChanged
                  << " mush=" << extremeCount
                  << " worstEnvFit=" << minEnvFit << std::endl;
    }

    // --- host transport sync -------------------------------------------------
    // Sync is on by default: rolling the DAW starts the tape, stopping it stops
    // the tape.  A manual REC must survive while the DAW is parked.
    bool syncTransportOk = true;
    {
        MockPlayHead head;
        proc.setPlayHead (&head);

        auto setP = [&] (const char* id, float v)
        {
            if (auto* p = proc.apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };

        setP ("transport", 0.0f);
        renderSeconds (proc, 0.05, peak);
        const bool idleBefore = ! proc.getDisplayPlaying();

        head.playing = true;
        renderSeconds (proc, 0.10, peak);
        const bool startedByHost = proc.getDisplayPlaying();

        head.playing = false;
        renderSeconds (proc, 0.10, peak);
        const bool stoppedByHost = ! proc.getDisplayPlaying();

        // Manual REC while the host is stopped must not be cancelled.
        setP ("transport", 2.0f);
        renderSeconds (proc, 0.20, peak);
        const bool manualRec = proc.getDisplayRecording();

        setP ("transport", 0.0f);
        renderSeconds (proc, 0.05, peak);

        syncTransportOk = idleBefore && startedByHost && stoppedByHost && manualRec;

        if (! syncTransportOk)
            std::cout << "sync idle=" << idleBefore << " start=" << startedByHost
                      << " stop=" << stoppedByHost << " rec=" << manualRec << std::endl;

        proc.setPlayHead (nullptr);
    }

    // --- state round-trip ----------------------------------------------------
    if (auto* p = proc.apvts.getParameter ("mix"))
        p->setValueNotifyingHost (p->convertTo0to1 (0.42f));

    juce::MemoryBlock state;
    proc.getStateInformation (state);

    if (auto* p = proc.apvts.getParameter ("mix"))
        p->setValueNotifyingHost (p->convertTo0to1 (0.10f));

    proc.setStateInformation (state.getData(), (int) state.getSize());

    const bool stateOk = std::abs (get ("mix") - 0.42f) < 0.001f;

    // --- editor render -------------------------------------------------------
    // Mark the state as restored so the editor does not apply the default
    // preset (which would change the parameters we just checked).
    bool editorOk = true;
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        if (editor == nullptr)
        {
            editorOk = false;
        }
        else
        {
            editor->setSize (editor->getWidth(), editor->getHeight());

            // A real host shows the editor before it paints; without this the
            // tape map's own timer (which skips work while hidden) never fills
            // the waveform cache and the offscreen render is blank.
            editor->setVisible (true);

            // The tape map fills its waveform cache from its own 30 Hz timer, so
            // the message loop has to run before the first paint: otherwise the
            // rendered image has an empty screen and proves nothing.  HISS is
            // raised half-way through, which is the case that used to paint far
            // outside the screen.
            const float savedHiss = get ("hiss");

            pumpMessages (200);

            if (auto* p = proc.apvts.getParameter ("hiss"))
                p->setValueNotifyingHost (p->convertTo0to1 (1.0f));

            pumpMessages (120);

            juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
            {
                juce::Graphics g (img);
                editor->paintEntireComponent (g, true);
            }

            if (auto* p = proc.apvts.getParameter ("hiss"))
                p->setValueNotifyingHost (p->convertTo0to1 (savedHiss));

            editorOk = img.getWidth() > 0 && img.getHeight() > 0;

            // The single-display layout must stay compact enough for a laptop.
            editorOk = editorOk && editor->getWidth() <= 1180
                                 && editor->getHeight() <= 780;

            // The map has to actually draw the take.  A blank screen used to look
            // like a successful render (the timer never ran), so check pixels: the
            // wave is normalised to the panel, so the top band of the map must
            // contain material in both themes.
            bool mapDrawn = false;
            {
                const juce::Image::BitmapData data (img, juce::Image::BitmapData::readOnly);

                const int x0 = juce::jlimit (0, data.width - 1, 160);
                const int x1 = juce::jlimit (x0 + 1, data.width, 960);
                const int y0 = juce::jlimit (0, data.height - 1, 150);
                const int y1 = juce::jlimit (y0 + 1, data.height, 310);

                // The scope background is the colour just inside the map corner:
                // the wave is normalised to 92% of the panel, so the outer 4%
                // margin is always background.  (Using the most common colour
                // here was wrong - a dense wave covers more than half the map.)
                const auto bg = data.getPixelColour (x0 + 3, y0 + 3);
                const int br = bg.getRed(), bgG = bg.getGreen(), bb = bg.getBlue();

                // Top band of the map: only a normalised wave reaches up here.
                int hits = 0;
                for (int y = y0; y < juce::jmin (y1, y0 + 40); ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        const auto c = data.getPixelColour (x, y);
                        const int d = std::abs ((int) c.getRed() - br)
                                    + std::abs ((int) c.getGreen() - bgG)
                                    + std::abs ((int) c.getBlue() - bb);
                        if (d > 60)
                            ++hits;
                    }

                mapDrawn = hits > 800;

                if (! mapDrawn)
                    std::cout << "map blank: hits=" << hits << std::endl;
            }

            editorOk = editorOk && mapDrawn;

            std::cout << "editorSize=" << editor->getWidth() << "x"
                      << editor->getHeight() << std::endl;

            auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("somnia_editor.png");
            out.deleteFile();

            bool pngOk = false;
            {
                juce::FileOutputStream os (out);
                if (os.openedOk())
                {
                    juce::PNGImageFormat png;
                    pngOk = png.writeImageToStream (img, os);
                    os.flush();
                }
            }

            if (! pngOk)
                std::cout << "png write failed: " << out.getFullPathName() << std::endl;
        }
    }

    proc.releaseResources();

    // --- theme contrast ------------------------------------------------------
    // The tape map is the instrument's main feedback surface: the wave (accent)
    // has to stand out from the scope behind it in *both* themes.  The light
    // theme used to share the dark near-black wash, which left the dark violet
    // wave sitting on mid grey with barely any separation.
    bool contrastOk = true;
    {
        somnia::SomniaLookAndFeel lf;

        auto luminance = [] (juce::Colour c)
        {
            auto lin = [] (float v)
            {
                return v <= 0.03928f ? v / 12.92f
                                     : std::pow ((v + 0.055f) / 1.055f, 2.4f);
            };

            return 0.2126f * lin (c.getFloatRed())
                 + 0.7152f * lin (c.getFloatGreen())
                 + 0.0722f * lin (c.getFloatBlue());
        };

        auto contrast = [&luminance] (juce::Colour a, juce::Colour b)
        {
            const float la = luminance (a);
            const float lb = luminance (b);
            return (juce::jmax (la, lb) + 0.05f) / (juce::jmin (la, lb) + 0.05f);
        };

        for (const bool dark : { true, false })
        {
            lf.setDark (dark);

            const auto scope = lf.findColour (somnia::SomniaLookAndFeel::scopeColourId);
            const auto wave  = lf.findColour (somnia::SomniaLookAndFeel::accentColourId);
            const float ratio = contrast (scope, wave);

            if (ratio < 4.5f)
            {
                contrastOk = false;
                std::cout << "low wave contrast: dark=" << dark
                          << " ratio=" << ratio << std::endl;
            }
        }
    }

    std::cout << "defaults=" << defaultsOk
              << " freeLen=" << freeLen << " free=" << freeOk
              << " recLevel=" << recLevelOk
              << " overdubLevel=" << overdubLevelOk
              << " play=" << playOk << " finite=" << finite
              << " pingPong=" << pingPongOk
              << " release=" << releaseOk
              << " syncTransport=" << syncTransportOk
              << " syncLen=" << syncLen << " sync=" << syncOk
              << " waveform=" << waveformOk
              << " fullPass=" << fullPassOk
              << " random=" << randomOk << " state=" << stateOk
              << " editor=" << editorOk
              << " musical=" << musicalOk
              << " contrast=" << contrastOk
              << " mixZero=" << mixZeroOk << std::endl;

    if (! (defaultsOk && freeOk && recLevelOk && overdubLevelOk && playOk
           && finite && pingPongOk && releaseOk && syncTransportOk && syncOk
           && waveformOk && fullPassOk && randomOk && stateOk && editorOk
           && musicalOk && contrastOk && mixZeroOk))
    {
        std::cout << "FAIL: plugin integration" << std::endl;
        return 1;
    }

    std::cout << "PASS: plugin integration" << std::endl;
    return 0;
}
