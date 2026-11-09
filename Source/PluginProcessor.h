#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <vector>

#include "dsp/TapeLoop.h"
#include "dsp/TapeDegrader.h"
#include "dsp/Adsr.h"
#include "dsp/Limiter.h"
#include "DebugLogger.h"

//==============================================================================
namespace somnia
{
    namespace ids
    {
        static constexpr const char* mix        = "mix";
        static constexpr const char* loopLength = "loopLength";
        static constexpr const char* loopStart  = "loopStart";
        static constexpr const char* loopEnd    = "loopEnd";
        static constexpr const char* speed      = "speed";
        static constexpr const char* direction  = "direction";
        static constexpr const char* feedback   = "feedback";
        static constexpr const char* attack     = "attack";
        static constexpr const char* decay      = "decay";
        static constexpr const char* sustain    = "sustain";
        static constexpr const char* release    = "release";
        static constexpr const char* wear       = "wear";
        static constexpr const char* dropouts   = "dropouts";
        static constexpr const char* splice     = "splice";
        static constexpr const char* oxide      = "oxide";
        static constexpr const char* wow        = "wow";
        static constexpr const char* hiss       = "hiss";
        static constexpr const char* tone       = "tone";
        static constexpr const char* sat        = "sat";
        static constexpr const char* output     = "output";
        static constexpr const char* limiter    = "limiter";
        static constexpr const char* transport  = "transport";
        static constexpr const char* syncTransport = "syncTransport";
    }

    // Loop lengths in whole notes (4 beats each in 4/4).  Choice 0 is "Free"
    // (record until STOP).
    static constexpr double kLoopBeats[] = { 1.0, 2.0, 4.0, 8.0, 16.0 };
}

//==============================================================================
class SomniaAudioProcessor : public juce::AudioProcessor
{
public:
    SomniaAudioProcessor();
    ~SomniaAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 60.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- message-thread actions (editor buttons) -----------------------------
    void randomizeParameters();
    void setTransport (int mode);          // 0 = Stop, 1 = Play, 2 = Rec
    void clearLoop();
    void rewindLoop();
    void fastForwardLoop();
    void nudgeSpeed (float semitones);     // SLOW / FAST: shift the varispeed knob

    // --- tape display (audio thread -> message thread) -----------------------
    float getDisplayLoopPosition() const noexcept
    {
        return displayLoopPos.load (std::memory_order_relaxed);
    }

    float getDisplayLoopLengthSeconds() const noexcept
    {
        return displayLoopLen.load (std::memory_order_relaxed);
    }

    float getDisplayTapeLengthSeconds() const noexcept
    {
        return displayTapeLen.load (std::memory_order_relaxed);
    }

    // True once something has actually been recorded onto the tape.
    bool getDisplayHasContent() const noexcept
    {
        return displayHasContent.load (std::memory_order_relaxed) > 0.5f;
    }

    // True while a brand-new take is being written; the read head is idle then,
    // so the UI animates the reels from the recording itself.
    bool getDisplayFreshRecording() const noexcept
    {
        return displayFreshRecording.load (std::memory_order_relaxed) > 0.5f;
    }

    float getDisplayLoopStartSeconds() const noexcept
    {
        return displayLoopStart.load (std::memory_order_relaxed);
    }

    float getDisplayLoopEndSeconds() const noexcept
    {
        return displayLoopEnd.load (std::memory_order_relaxed);
    }

    float getDisplayWear() const noexcept
    {
        return displayWear.load (std::memory_order_relaxed);
    }

    float getDisplayDropout() const noexcept
    {
        return displayDropout.load (std::memory_order_relaxed);
    }

    int getDisplayCycles() const noexcept
    {
        return displayCycles.load (std::memory_order_relaxed);
    }

    bool getDisplayRecording() const noexcept
    {
        return displayRecording.load (std::memory_order_relaxed) > 0.5f;
    }

    bool getDisplayPlaying() const noexcept
    {
        return displayPlaying.load (std::memory_order_relaxed) > 0.5f;
    }

    float getDisplayGrDb() const noexcept
    {
        return displayGrDb.load (std::memory_order_relaxed);
    }

    bool getDisplayLimiting() const noexcept
    {
        return displayLimiting.load (std::memory_order_relaxed) > 0.5f;
    }

    float getDisplayBpm() const noexcept
    {
        return displayBpm.load (std::memory_order_relaxed);
    }

    // --- loop envelope (ADSR) telemetry --------------------------------------
    float getDisplayEnvelopeLevel() const noexcept
    {
        return displayEnvLevel.load (std::memory_order_relaxed);
    }

    // 0 = idle, 1 = attack, 2 = decay, 3 = sustain, 4 = release.
    int getDisplayEnvelopeStage() const noexcept
    {
        return displayEnvStage.load (std::memory_order_relaxed);
    }

    int getDirectionSign() const noexcept
    {
        return displayDirection.load (std::memory_order_relaxed);
    }

    // Decimated waveform of the whole recorded tape for the tape map.
    void copyLoopWaveform (float* minOut, float* maxOut, int numBuckets) const
    {
        tapeLoop.copyWaveform (minOut, maxOut, numBuckets);
    }

    // Changes whenever new material is written, so the map can refresh the
    // waveform even when the tape length is fixed (synced mode).
    std::uint32_t getDisplayWriteVersion() const noexcept
    {
        return tapeLoop.getWriteVersion();
    }

    // "Black box" recorder: per-stage level snapshots + anomaly dumps.
    somnia::DebugLogger& getDebugLogger() noexcept { return debugLogger; }

    bool wasStateRestored() const noexcept
    {
        return stateRestored.load (std::memory_order_relaxed);
    }

    bool wasDefaultPresetApplied() const noexcept
    {
        return defaultPresetApplied.load (std::memory_order_relaxed);
    }

    void markDefaultPresetApplied() noexcept
    {
        defaultPresetApplied.store (true, std::memory_order_relaxed);
    }

    const juce::String& getLastPresetName() const noexcept { return lastPresetName; }
    void setLastPresetName (const juce::String& n) { lastPresetName = n; }

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    struct RawParams
    {
        std::atomic<float>* mix        = nullptr;
        std::atomic<float>* loopLength = nullptr;
        std::atomic<float>* loopStart  = nullptr;
        std::atomic<float>* loopEnd    = nullptr;
        std::atomic<float>* speed      = nullptr;
        std::atomic<float>* direction  = nullptr;
        std::atomic<float>* feedback   = nullptr;
        std::atomic<float>* attack     = nullptr;
        std::atomic<float>* decay      = nullptr;
        std::atomic<float>* sustain    = nullptr;
        std::atomic<float>* release    = nullptr;
        std::atomic<float>* wear       = nullptr;
        std::atomic<float>* dropouts   = nullptr;
        std::atomic<float>* splice     = nullptr;
        std::atomic<float>* oxide      = nullptr;
        std::atomic<float>* wow        = nullptr;
        std::atomic<float>* hiss       = nullptr;
        std::atomic<float>* tone       = nullptr;
        std::atomic<float>* sat        = nullptr;
        std::atomic<float>* output     = nullptr;
        std::atomic<float>* limiter    = nullptr;
        std::atomic<float>* transport  = nullptr;
        std::atomic<float>* syncTransport = nullptr;
    };

    void bindRawParams();
    void applyTransport (int mode);
    void updateLoopLength();
    void applyLoopBounds();
    void resetDsp();

    RawParams rawParams;

    double currentBpm = 120.0;
    double currentSampleRate = 48000.0;
    int    lastTransport = -1;

    // The transport parameter is never written from the audio thread, so the
    // state the host transitions produce has to be latched here.  Without the
    // latch the parameter (still "Stop") overrode a host-started PLAY on the
    // very next block.
    int    effectiveTransport = 0;
    int    lastParamTransport = -1;
    std::atomic<int> pendingTransport { -1 };

    // Sync Transport reacts to host transport *transitions*, not to its steady
    // state, so manual REC/STOP still work while the DAW is parked.
    bool   lastHostPlaying = false;
    bool   hostTransportSeen = false;

    somnia::TapeLoop     tapeLoop;
    somnia::TapeDegrader tapeDegrader;
    somnia::Adsr         loopAdsr;
    somnia::Limiter      outputLimiter;

    somnia::DebugLogger debugLogger;
    double logTimeSec = 0.0;

    juce::AudioBuffer<float> loopBuffer;
    juce::AudioBuffer<float> wetBuffer;
    juce::AudioBuffer<float> outBuffer;

    juce::SmoothedValue<float> mixSmoothed, outputSmoothed;

    // Varispeed is applied as a playback rate; ramping it over a few blocks is
    // what makes SLOW / FAST sound like a tape easing down instead of a jump.
    juce::SmoothedValue<float> speedSmoothed;

    juce::Random rng;

    std::atomic<bool> clearRequested { false };
    std::atomic<int>  rewindRequests { 0 };
    std::atomic<int>  forwardRequests { 0 };


    std::atomic<float> displayLoopPos   { 0.0f };
    std::atomic<float> displayLoopLen   { 0.0f };
    std::atomic<float> displayTapeLen   { 0.0f };
    std::atomic<float> displayLoopStart { 0.0f };
    std::atomic<float> displayLoopEnd   { 0.0f };
    std::atomic<float> displayHasContent { 0.0f };
    std::atomic<float> displayFreshRecording { 0.0f };
    std::atomic<float> displayWear      { 0.0f };
    std::atomic<float> displayDropout   { 1.0f };
    std::atomic<int>   displayCycles    { 0 };
    std::atomic<int>   displayDirection { 1 };
    std::atomic<float> displayRecording { 0.0f };
    std::atomic<float> displayPlaying   { 0.0f };
    std::atomic<float> displayGrDb      { 0.0f };
    std::atomic<float> displayLimiting  { 0.0f };
    std::atomic<float> displayBpm       { 120.0f };
    std::atomic<float> displayEnvLevel  { 0.0f };
    std::atomic<int>   displayEnvStage  { 0 };

    std::atomic<bool> stateRestored { false };
    std::atomic<bool> defaultPresetApplied { false };

    juce::String lastPresetName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SomniaAudioProcessor)
};
