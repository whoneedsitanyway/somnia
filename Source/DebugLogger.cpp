#include "DebugLogger.h"
#include "PresetManager.h"

namespace somnia
{

//==============================================================================
DebugLogger::DebugLogger()
    : juce::Thread ("Somnia debug logger")
{
}

DebugLogger::~DebugLogger()
{
    stop();
}

void DebugLogger::start()
{
    if (! isThreadRunning())
    {
        if (! sessionStarted)
        {
            clearLog();
            sessionStarted = true;
        }

        startThread (juce::Thread::Priority::low);
    }
}

void DebugLogger::stop()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        notify();
        stopThread (1000);
    }
}

//==============================================================================
void DebugLogger::push (const BlockSnapshot& s) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 > 0)
    {
        fifoData[(size_t) start1] = s;
        fifo.finishedWrite (1);
    }
}

//==============================================================================
juce::File DebugLogger::getLogFile() const
{
   #ifdef SOMNIA_TEST_BUILD
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("somnia_debug.log");
   #else
    return PresetManager::getAppDir().getChildFile ("somnia_debug.log");
   #endif
}

void DebugLogger::clearLog()
{
    auto f = getLogFile();
    if (f.existsAsFile())
        f.deleteFile();

    fileStarted = false;
    lastLoggedSec = -1.0e9;
    lastLoggedFlags = 0;
    suppressedSinceLastLog = 0;
}

//==============================================================================
void DebugLogger::run()
{
    while (! threadShouldExit())
    {
        drain();
        wait (50);
    }
    drain();
}

void DebugLogger::drain()
{
    for (int guard = 0; guard < kFifoSize; ++guard)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);

        if (size1 <= 0)
            break;

        const BlockSnapshot s = fifoData[(size_t) start1];
        fifo.finishedRead (1);

        history[(size_t) historyWrite] = s;
        historyWrite = (historyWrite + 1) % kHistory;
        if (historyCount < kHistory)
            ++historyCount;

        if (s.flags != 0)
        {
            lastFlags.store (s.flags, std::memory_order_relaxed);
            lastAnomalySec.store (s.timeSec, std::memory_order_relaxed);
            anomalyCount.fetch_add (1, std::memory_order_relaxed);

            const bool flagsChanged    = (s.flags != lastLoggedFlags);
            const bool cooldownElapsed = (s.timeSec - lastLoggedSec >= kAnomalyCooldownSec);
            const bool firstEver       = (lastLoggedSec < -1.0e8);

            if (firstEver || flagsChanged || cooldownElapsed)
            {
                juce::String batch;
                batch << "\n"
                      << "============================================================\n"
                      << "*** ANOMALY @" << timestamp() << "  flags: "
                      << describeFlags (s.flags) << "\n"
                      << "============================================================\n";

                if (suppressedSinceLastLog > 0)
                    batch << "  (+" << suppressedSinceLastLog
                          << " similar block(s) suppressed since the last entry)\n";

                const int n = historyCount;
                for (int i = 0; i < n; ++i)
                {
                    const int idx = (historyWrite - n + i + kHistory * 2) % kHistory;
                    batch << formatLine (history[(size_t) idx], true) << "\n";
                }

                batch << "---- trigger block ----\n";
                batch << formatLine (s, true) << "\n";
                appendLine (batch);

                lastLoggedFlags = s.flags;
                lastLoggedSec = s.timeSec;
                suppressedSinceLastLog = 0;
            }
            else
            {
                ++suppressedSinceLastLog;
            }
        }
        else if (s.timeSec - lastHeartbeatSec >= 2.0)
        {
            lastHeartbeatSec = s.timeSec;
            appendLine (formatLine (s, false) + "\n");
        }
    }
}

//==============================================================================
void DebugLogger::appendLine (const juce::String& line)
{
    auto f = getLogFile();
    f.getParentDirectory().createDirectory();

    if (f.existsAsFile() && f.getSize() > kMaxLogBytes)
    {
        auto prev = f.getSiblingFile ("somnia_debug.prev.log");
        prev.deleteFile();
        f.moveFileTo (prev);
        fileStarted = false;
    }

    if (! fileStarted)
    {
        f.appendText (juce::String ("# Somnia debug log - started ")
                          + timestamp() + "\n");
        fileStarted = true;
    }

    f.appendText (line);
}

//==============================================================================
juce::String DebugLogger::timestamp()
{
    const auto now = juce::Time::getCurrentTime();
    return now.toString (true, true, true, true) + "."
             + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
}

juce::String DebugLogger::describeFlags (unsigned flags)
{
    if (flags == 0)
        return "none";

    juce::StringArray parts;
    if (flags & kFlagNonFinite) parts.add ("NONFINITE");
    if (flags & kFlagInputHot)  parts.add ("INPUT_HOT");
    if (flags & kFlagLoopHot)   parts.add ("LOOP_HOT");
    if (flags & kFlagOutHot)    parts.add ("OUT_HOT");
    if (flags & kFlagHeavyGR)   parts.add ("HEAVY_GR");
    return parts.joinIntoString ("|");
}

juce::String DebugLogger::formatLine (const BlockSnapshot& s, bool anomaly)
{
    juce::String line;
    line << (anomaly ? "  " : "")
         << "t=" << juce::String (s.timeSec, 2) << "s"
         << "  in=" << juce::String (s.inPeak, 4)
         << " loop=" << juce::String (s.loopPeak, 4)
         << " deg=" << juce::String (s.degPeak, 4)
         << " wet=" << juce::String (s.wetPeak, 4)
         << " mix=" << juce::String (s.mixPeak, 4)
         << " out=" << juce::String (s.outPeak, 4)
         << "  GR=" << juce::String (s.grDb, 1) << "dB"
         << "  fb=" << juce::String (s.feedback, 2)
         << " wear=" << juce::String (s.wear, 2)
         << " drop=" << juce::String (s.dropouts, 2)
         << " splice=" << juce::String (s.splice, 2)
         << " oxide=" << juce::String (s.oxide, 2)
         << " wow=" << juce::String (s.wow, 2)
         << " hiss=" << juce::String (s.hiss, 2)
         << " tone=" << juce::String (s.tone, 2)
         << " sat=" << juce::String (s.sat, 2)
         << " speed=" << juce::String (s.speed, 2)
         << " out=" << juce::String (s.output, 1)
         << " dir=" << s.direction
         << " len=" << s.loopChoice
         << " cyc=" << s.cycles
         << " REC=" << (s.recording ? 1 : 0)
         << " PLAY=" << (s.playing ? 1 : 0)
         << " FREE=" << (s.freeMode ? 1 : 0)
         << " LIM=" << (s.limiterOn ? 1 : 0);
    return line;
}

} // namespace somnia
