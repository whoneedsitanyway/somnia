#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

namespace somnia
{

//==============================================================================
//  Lightweight "black box" recorder for hunting rare, hard-to-reproduce audio
//  bugs (runaway feedback, a stuck read head, NaN bursts).  The audio thread
//  only pushes small POD snapshots into a lock-free FIFO; a background thread
//  drains it, keeps a rolling history and appends to a log file.  When an
//  anomaly flag is set the whole recent history is dumped.
//==============================================================================
struct BlockSnapshot
{
    double timeSec = 0.0;

    // Per-stage peak magnitudes.
    float inPeak   = 0.0f;
    float loopPeak = 0.0f;
    float degPeak  = 0.0f;
    float wetPeak  = 0.0f;
    float mixPeak  = 0.0f;
    float outPeak  = 0.0f;

    float grDb = 0.0f;

    // Key parameters in effect for this block.
    float mix = 0.0f, feedback = 0.0f, wear = 0.0f, dropouts = 0.0f,
          splice = 0.0f, oxide = 0.0f, wow = 0.0f, hiss = 0.0f,
          tone = 0.0f, sat = 0.0f, speed = 0.0f, output = 0.0f;

    int  direction = 0;
    int  loopChoice = 0;
    int  cycles = 0;
    bool recording = false;
    bool playing = false;
    bool freeMode = false;
    bool limiterOn = false;

    unsigned flags = 0;
};

enum SnapshotFlags : unsigned
{
    kFlagNonFinite = 1u << 0,   // NaN / Inf anywhere in the chain
    kFlagInputHot  = 1u << 1,   // input peak > 2.0
    kFlagLoopHot   = 1u << 2,   // loop / feedback runaway
    kFlagOutHot    = 1u << 3,   // final output peak > 1.5 (protection failed)
    kFlagHeavyGR   = 1u << 4    // limiter reducing more than 24 dB
};

class DebugLogger : private juce::Thread
{
public:
    DebugLogger();
    ~DebugLogger() override;

    void start();
    void stop();

    // ---- audio thread -------------------------------------------------------
    void push (const BlockSnapshot& s) noexcept;

    // ---- message thread -----------------------------------------------------
    juce::File getLogFile() const;

    bool hasAnomaly() const noexcept
    {
        return anomalyCount.load (std::memory_order_relaxed) > 0;
    }

    void clearAnomaly() noexcept
    {
        anomalyCount.store (0, std::memory_order_relaxed);
        lastFlags.store (0, std::memory_order_relaxed);
    }

    void clearLog();

    static juce::String describeFlags (unsigned flags);

private:
    void run() override;
    void drain();
    void appendLine (const juce::String& line);
    static juce::String formatLine (const BlockSnapshot& s, bool anomaly);
    static juce::String timestamp();

    static constexpr int kFifoSize = 8192;
    static constexpr int kHistory  = 256;
    static constexpr int kMaxLogBytes = 4 * 1024 * 1024;
    static constexpr double kAnomalyCooldownSec = 5.0;

    juce::AbstractFifo fifo { kFifoSize };
    std::array<BlockSnapshot, kFifoSize> fifoData;

    std::array<BlockSnapshot, kHistory> history {};
    int    historyCount = 0;
    int    historyWrite = 0;
    double lastHeartbeatSec = -1000.0;
    bool   fileStarted = false;
    bool   sessionStarted = false;

    double   lastLoggedSec = -1.0e9;
    unsigned lastLoggedFlags = 0;
    int      suppressedSinceLastLog = 0;

    std::atomic<int>      anomalyCount { 0 };
    std::atomic<unsigned> lastFlags { 0 };
    std::atomic<double>   lastAnomalySec { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugLogger)
};

} // namespace somnia
