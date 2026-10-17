#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <cmath>
#include <deque>
#include <functional>

//==============================================================================
// Sound diagnostics: why did the bench stay silent this time?
//
// The bench sometimes produces no sound and the cause is gone by the time anyone looks. This records what the
// audio path was doing, all the time, into %APPDATA%\VstTestBench\diag\diag_<start>.txt:
//
//   - AudioProbe: counters the audio thread updates (callbacks, block time, device output peak, non-finite
//     samples, MIDI that reached the player, device start / stop / error).
//   - SoundDiag:  the message thread hands it one Frame per timer tick (100 ms). It writes one status line per
//     second while anything happens (a heartbeat every 30 s while idle), logs every change of the device and
//     every status text, and detects incidents:
//        STALL    the device says it is playing but the audio callback stopped
//        SILENT   notes came in (or the file plays) and nothing reached the device output
//        NONFINITE / OVERLOAD / XRUN / DEVICE ERROR
//     An incident line names the first stage along the path where the signal disappeared, then a detail block.
//   - A watchdog thread logs when the message thread stops ticking (a hung UI also stops the meters).
// The user's "No sound!" button writes a MARK with the same diagnosis over the last 3 seconds.
//==============================================================================

// Audio-thread side. Every field is an atomic; the audio thread writes, the message thread reads.
struct AudioProbe
{
    std::atomic<juce::uint64> callbacks { 0 }, samples { 0 };
    std::atomic<float> outPeak { 0.0f };           // device output, every channel, since the previous take
    std::atomic<float> maxBlockMs { 0.0f };        // longest callback since the previous take
    std::atomic<juce::uint32> overBudget { 0 };    // callbacks that took longer than their own duration
    std::atomic<juce::uint32> nonFinite { 0 };     // blocks with NaN / Inf in the device output
    std::atomic<juce::uint32> midiHw { 0 }, midiHwNotes { 0 };   // from enabled MIDI inputs, at the player
    std::atomic<juce::uint32> midiInj { 0 }, midiInjNotes { 0 }; // injected (control server, MIDI file play)
    std::atomic<juce::uint32> noteChannels { 0 };  // bit c-1: a note-on came on channel c, since the previous take
    std::atomic<juce::uint32> starts { 0 }, stops { 0 }, errors { 0 };
    std::atomic<juce::uint32> lastCallbackMs { 0 };
    juce::SpinLock errorLock;
    juce::String lastError;                        // under errorLock

    float takeOutPeak()    { return outPeak.exchange (0.0f); }
    float takeMaxBlockMs() { return maxBlockMs.exchange (0.0f); }

    static void raiseMax (std::atomic<float>& a, float v)
    {
        float cur = a.load();
        while (v > cur && ! a.compare_exchange_weak (cur, v)) {}
    }

    // Audio thread: after the player filled the device buffers.
    void onBlock (float* const* out, int nOut, int numSamples, double seconds, double sampleRate)
    {
        float pk = 0.0f;
        bool bad = false;
        for (int c = 0; c < nOut; ++c)
            if (const float* d = out[c])
                for (int i = 0; i < numSamples; ++i)
                {
                    const float v = d[i];
                    if (! std::isfinite (v)) { bad = true; continue; }
                    pk = juce::jmax (pk, std::abs (v));
                }
        raiseMax (outPeak, pk);
        if (bad) nonFinite.fetch_add (1);
        const float ms = (float) (seconds * 1000.0);
        raiseMax (maxBlockMs, ms);
        if (sampleRate > 0.0 && seconds > numSamples / sampleRate)
            overBudget.fetch_add (1);
        callbacks.fetch_add (1);
        samples.fetch_add ((juce::uint64) numSamples);
        lastCallbackMs.store (juce::Time::getMillisecondCounter());
    }

    void onError (const juce::String& e)
    {
        { const juce::SpinLock::ScopedLockType sl (errorLock); lastError = e; }
        errors.fetch_add (1);
    }
    juce::String getLastError() { const juce::SpinLock::ScopedLockType sl (errorLock); return lastError; }
};

//==============================================================================
class SoundDiag : private juce::Thread
{
public:
    // Stages in signal order, as the frame carries them.
    enum Stage { stAudio = 0, stInst1, stInst2, stInst3, stRet1, stRet2, stMaster, numStages };
    static const char* stageName (int s)
    {
        static const char* n[] = { "AUDIO", "INST1", "INST2", "INST3", "RET1", "RET2", "MASTER" };
        return n[s];
    }

    // What the message thread sees each tick. Counters are cumulative; peaks are maxima since the previous tick.
    struct Frame
    {
        juce::uint32 ms = 0;
        // device
        bool devOpen = false, devPlaying = false;
        juce::String devType, devName;
        double sampleRate = 0.0;
        int buffer = 0, outChans = 0, inChans = 0, xruns = -1;
        double cpu = 0.0;
        // probe (copied)
        juce::uint64 callbacks = 0;
        float devPeak = 0.0f, maxBlockMs = 0.0f;
        juce::uint32 overBudget = 0, nonFinite = 0, midiHw = 0, midiHwNotes = 0, midiInj = 0, midiInjNotes = 0;
        juce::uint32 starts = 0, stops = 0, errors = 0;
        juce::uint32 noteChannels = 0;             // bit c-1: note-ons on channel c since the previous tick
        juce::String lastError;
        // graph
        float stIn[numStages] {}, stOut[numStages] {};
        bool stSounding[numStages] {};             // strip flags: not muted, not solo-silenced, not mode-silenced
        bool stPresent[numStages] {};
        bool stModeOff[numStages] {};              // silenced by the source-mode preset (by design, not a cause)              // track has a source (file / input / instrument / bus FX)
        juce::uint32 instSeen[3] {}, instPassed[3] {};   // note-ons at each INST track's MIDI filter (cumulative)
        int sourceMode = 0;                        // 1 live, 2 VSTi, 3 file
        bool filePlaying = false, fileLoaded = false, preRender = false, cacheInPath = false, transportPlaying = false;
        int connections = 0;
    };

    // Detail block for incidents and marks (graph, plug-ins, MIDI inputs ...), built by the owner.
    std::function<juce::String()> describe;

    SoundDiag() : juce::Thread ("sound-diag-watchdog") {}
    ~SoundDiag() override { stop(); }

    void start (const juce::File& dir)
    {
        dir.createDirectory();
        pruneOld (dir, 30);
        logFile = dir.getChildFile ("diag_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S") + ".txt");
        {
            const juce::ScopedLock sl (fileLock);
            stream = std::make_unique<juce::FileOutputStream> (logFile);
            if (stream->failedToOpen()) stream.reset();
        }
        write ("=== VST TestBench sound diagnostics, " + juce::Time::getCurrentTime().toString (true, true, true, true)
               + " (" + juce::SystemStats::getOperatingSystemName() + ") ===");
        heartbeatMs.store (juce::Time::getMillisecondCounter());
        startThread (juce::Thread::Priority::low);
    }

    void stop()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            notify();
            stopThread (2000);
            write ("=== closed ===");
        }
        const juce::ScopedLock sl (fileLock);
        stream.reset();
    }

    juce::File getLogFile() const { return logFile; }
    int getIncidents() const { return incidents; }
    juce::String getLastIncident() const { return lastIncident; }
    juce::String getSummary() const { return summary; }

    // Any thread.
    void event (const juce::String& text) { write ("EVENT " + text); }

    // Message thread, once per timer tick.
    void push (const Frame& f)
    {
        heartbeatMs.store (f.ms);
        armed.store (true);
        if (! hasPrev) { prev = f; hasPrev = true; lastCbAdvanceMs = f.ms; secondStartMs = f.ms; logChanges (f, true); }
        logChanges (f, false);

        // --- per-tick bookkeeping ---
        const juce::uint32 newNotes = (f.midiHwNotes - prev.midiHwNotes) + (f.midiInjNotes - prev.midiInjNotes);
        if (f.callbacks != prev.callbacks) lastCbAdvanceMs = f.ms;
        accumulate (sec, f, prev);
        window.push_back ({ f.ms, f, prev });
        while (! window.empty() && f.ms - window.front().ms > 3000) window.pop_front();

        // --- STALL: playing device, no callbacks ---
        const bool stalled = f.devOpen && f.devPlaying && f.ms - lastCbAdvanceMs > 600;
        if (stalled && ! stallReported)
        {
            stallReported = true;
            incident ("STALL", "audio callback stopped " + juce::String (f.ms - lastCbAdvanceMs) + " ms ago while the device ("
                               + f.devType + " / " + f.devName + ") says it is playing", f);
        }
        else if (! stalled && stallReported)
        {
            stallReported = false;
            write ("RECOVER audio callbacks running again");
        }

        // --- SILENT: notes came in and nothing reached the device ---
        const float floor = kAudible;
        if (f.devPeak > floor) { notesPendingMs = 0; fileSilentMs = 0; silentReported = false; }
        // A silent spell ends after 5 s without notes: the next notes (perhaps after a settings change) are a new one.
        if (newNotes > 0) lastNoteMs = f.ms;
        if (notesPendingMs != 0 && newNotes == 0 && f.ms - lastNoteMs > 5000 && ! f.filePlaying)
        {
            notesPendingMs = 0;
            silentReported = false;
        }
        if (newNotes > 0 && notesPendingMs == 0 && f.devPeak <= floor)
            notesPendingMs = f.ms;
        if (notesPendingMs != 0 && ! silentReported && f.ms - notesPendingMs > 1500)
        {
            silentReported = true;
            incident ("SILENT", "notes arrived " + juce::String (f.ms - notesPendingMs) + " ms ago and nothing audible reached the device output", f, notesPendingMs);
        }
        if (f.filePlaying && f.devPeak <= floor) { if (fileSilentMs == 0) fileSilentMs = f.ms; }
        else if (! f.filePlaying) fileSilentMs = 0;
        if (fileSilentMs != 0 && ! silentReported && f.ms - fileSilentMs > 3000)
        {
            silentReported = true;
            incident ("SILENT", "the file plays and the device output has been silent for " + juce::String (f.ms - fileSilentMs)
                                + " ms (or the file is silent here)", f, fileSilentMs);
        }

        // --- per-tick anomalies ---
        if (f.nonFinite != prev.nonFinite)
            incident ("NONFINITE", juce::String (f.nonFinite - prev.nonFinite) + " block(s) with NaN/Inf in the device output", f);
        if (f.errors != prev.errors)
            incident ("DEVICE ERROR", f.lastError, f);
        if (f.starts != prev.starts) write ("EVENT device started (" + f.devType + " / " + f.devName + ")");
        if (f.stops != prev.stops)   write ("EVENT device stopped");

        // --- one line per second ---
        if (f.ms - secondStartMs >= 1000)
        {
            const bool active = sec.notes > 0 || sec.devPeak > kAudible || f.filePlaying || sec.overBudget > 0 || sec.xruns > 0
                                || f.ms - lastWrittenStatusMs >= 30000 || ! f.devPlaying;
            const auto line = statusLine (sec, f);
            summary = shortSummary (sec, f);
            if (sec.xruns > 0)      write ("XRUN " + juce::String (sec.xruns) + " in the last second");
            if (sec.overBudget > 0) write ("OVERLOAD " + juce::String (sec.overBudget) + " callback(s) took longer than their block (max "
                                           + juce::String (sec.maxBlockMs, 2) + " ms, block " + juce::String (blockMs (f), 2) + " ms)");
            if (active && (line != lastStatusBody || f.ms - lastWrittenStatusMs >= 30000 || sec.notes > 0 || sec.devPeak > kAudible))
            {
                write ("S " + line);
                lastWrittenStatusMs = f.ms;
            }
            lastStatusBody = line;
            sec = {};
            secondStartMs = f.ms;
        }
        prev = f;
    }

    // The user heard nothing: the same diagnosis over the last 3 seconds.
    void mark (const juce::String& note)
    {
        if (! hasPrev) { write ("MARK " + note + " (no frames yet)"); return; }
        Acc a;
        for (const auto& w : window) accumulate (a, w.f, w.prev);
        write ("MARK " + note);
        write ("  last 3 s: " + statusLine (a, prev));
        write ("  diagnosis: " + diagnose (a, prev));
        writeDetail();
    }

private:
    // "Sound came out" means above -60 dBFS. A lower floor counts a live input's noise (about -78 dBFS on the
    // built-in ASIO driver, measured) as sound, and then notes into an empty instrument never look silent.
    static constexpr float kAudible = 1.0e-3f;

    struct Acc
    {
        float devPeak = 0.0f, maxBlockMs = 0.0f;
        float in[numStages] {}, out[numStages] {};
        juce::uint32 callbacks = 0, overBudget = 0, notes = 0, hwNotes = 0, injNotes = 0, hwMsgs = 0, xruns = 0;
        juce::uint32 seen[3] {}, passed[3] {};
        juce::uint32 noteChannels = 0;
        bool anyFilePlaying = false;
    };
    struct WindowFrame { juce::uint32 ms; Frame f, prev; };

    static void accumulate (Acc& a, const Frame& f, const Frame& p)
    {
        a.devPeak = juce::jmax (a.devPeak, f.devPeak);
        a.maxBlockMs = juce::jmax (a.maxBlockMs, f.maxBlockMs);
        for (int s = 0; s < numStages; ++s) { a.in[s] = juce::jmax (a.in[s], f.stIn[s]); a.out[s] = juce::jmax (a.out[s], f.stOut[s]); }
        a.callbacks  += (juce::uint32) (f.callbacks - p.callbacks);
        a.overBudget += f.overBudget - p.overBudget;
        a.hwNotes    += f.midiHwNotes - p.midiHwNotes;
        a.injNotes   += f.midiInjNotes - p.midiInjNotes;
        a.hwMsgs     += f.midiHw - p.midiHw;
        a.notes       = a.hwNotes + a.injNotes;
        if (f.xruns > p.xruns && p.xruns >= 0) a.xruns += (juce::uint32) (f.xruns - p.xruns);
        for (int t = 0; t < 3; ++t) { a.seen[t] += f.instSeen[t] - p.instSeen[t]; a.passed[t] += f.instPassed[t] - p.instPassed[t]; }
        a.anyFilePlaying = a.anyFilePlaying || f.filePlaying;
        a.noteChannels |= f.noteChannels;
    }

    static juce::String channels (juce::uint32 m)
    {
        juce::StringArray c;
        for (int i = 0; i < 16; ++i) if (m & (1u << i)) c.add (juce::String (i + 1));
        return c.isEmpty() ? juce::String ("-") : c.joinIntoString (",");
    }

    static juce::String db (float v)
    {
        if (v <= 1.0e-6f) return "-inf";
        return juce::String (20.0f * std::log10 (v), 1);
    }
    static double blockMs (const Frame& f) { return f.sampleRate > 0.0 ? 1000.0 * f.buffer / f.sampleRate : 0.0; }
    static const char* modeName (int m) { return m == 1 ? "live" : m == 2 ? "VSTi" : m == 3 ? "file" : "?"; }

    juce::String statusLine (const Acc& a, const Frame& f) const
    {
        juce::String s;
        s << "dev=" << (f.devOpen ? (f.devPlaying ? "play" : "open") : "CLOSED")
          << " cb=" << (int) a.callbacks << " blk=" << juce::String (a.maxBlockMs, 2) << "/" << juce::String (blockMs (f), 2) << "ms"
          << " cpu=" << juce::String (f.cpu * 100.0, 1) << "%"
          << " | src=" << modeName (f.sourceMode)
          << (f.fileLoaded ? (f.filePlaying ? " file=play" : " file=stop") : "")
          << (f.preRender ? (f.cacheInPath ? " cache" : " prerender(nocache)") : "")
          << " | notes hw=" << (int) a.hwNotes << " inj=" << (int) a.injNotes;
        if (a.noteChannels != 0) s << " ch=" << channels (a.noteChannels);
        for (int t = 0; t < 3; ++t)
            if (f.stPresent[stInst1 + t])
                s << " i" << (t + 1) << "=" << (int) a.passed[t] << "/" << (int) a.seen[t];
        s << " | pk";
        for (int st = 0; st < numStages; ++st)
            if (f.stPresent[st] || st == stMaster)
                s << " " << stageName (st) << (f.stSounding[st] ? "" : "(off)") << "=" << db (a.in[st]) << ">" << db (a.out[st]);
        s << " DEV=" << db (a.devPeak);
        return s;
    }

    juce::String shortSummary (const Acc& a, const Frame& f) const
    {
        juce::String s;
        s << (f.devOpen ? (f.devPlaying ? "" : "DEV STOPPED ") : "NO DEVICE ")
          << "cb " << (int) a.callbacks << "/s  out " << db (a.devPeak) << " dB";
        if (incidents > 0) s << "  ! " << incidents;
        return s;
    }

    // The first place along the path where the signal is gone, with the evidence.
    juce::String diagnose (const Acc& a, const Frame& f) const
    {
        const float floor = kAudible;
        if (! f.devOpen)                return "no audio device is open";
        if (f.outChans == 0)            return "the device has no active output channels";
        if (a.callbacks == 0)           return "the audio callback is not running (device stalled or stopped)";
        if (a.devPeak > floor)          return "the device output had signal (" + db (a.devPeak) + " dBFS): the problem is after the bench (device routing / monitor / volume)";
        if (a.out[stMaster] > floor)    return "the master strip outputs signal but the device output is silent: the player or the device channel mapping drops it";
        if (a.in[stMaster] > floor && a.out[stMaster] < a.in[stMaster] * 0.1f) return juce::String ("signal reaches the master strip and the strip outputs nothing")
                                               + (f.stSounding[stMaster] ? " (gain at -100 dB?)" : " (master muted)");
        juce::StringArray why, byMode;
        for (int st = stAudio; st < stMaster; ++st)
        {
            if (! f.stPresent[st]) continue;
            if (f.stModeOff[st] && a.out[st] <= floor)
            {
                if (a.in[st] > floor) byMode.add (juce::String (stageName (st)) + " gets " + db (a.in[st]) + " dB, silenced by the source mode "
                                                  + modeName (f.sourceMode));
                continue;
            }
            if (a.out[st] > floor) why.add (juce::String (stageName (st)) + " outputs " + db (a.out[st]) + " dB but nothing reaches the master (routing / connections="
                                            + juce::String (f.connections) + ")");
            else if (a.in[st] > floor && a.out[st] < a.in[st] * 0.1f) why.add (juce::String (stageName (st)) + " gets " + db (a.in[st]) + " dB and its strip silences it"
                                                + (f.stSounding[st] ? " (gain?)" : " (mute / solo / source-mode preset)"));
        }
        if (! why.isEmpty()) return why.joinIntoString ("; ");
        if (! byMode.isEmpty() && a.notes == 0 && ! a.anyFilePlaying) return byMode.joinIntoString ("; ") + " (wrong source mode?)";

        // nothing anywhere: what should have made sound?
        juce::StringArray src;
        if (a.notes > 0)
        {
            bool anyInst = false;
            for (int t = 0; t < 3; ++t)
            {
                if (! f.stPresent[stInst1 + t]) continue;
                anyInst = true;
                if (a.seen[t] == 0)        src.add ("INST" + juce::String (t + 1) + ": no note reached its MIDI filter (its MIDI input device is another one?)");
                else if (a.passed[t] == 0) src.add ("INST" + juce::String (t + 1) + ": its MIDI channel mask dropped all " + juce::String ((int) a.seen[t])
                                                    + " notes (they came on ch " + channels (a.noteChannels) + ")");
                else                       src.add ("INST" + juce::String (t + 1) + ": got " + juce::String ((int) a.passed[t]) + " notes and produced no audio (the plug-in is silent)");
            }
            if (! anyInst) src.add ("notes arrived but no instrument is loaded");
        }
        else if (f.sourceMode == 2)
            src.add ("no MIDI note arrived at the player (hw msgs " + juce::String ((int) a.hwMsgs) + "; is the MIDI input enabled in Audio/MIDI Settings?)");
        if (f.sourceMode == 3 || a.anyFilePlaying)
        {
            if (! f.fileLoaded)          src.add ("file mode with no file loaded");
            else if (! a.anyFilePlaying) src.add ("the file player is stopped");
            else if (f.preRender && ! f.cacheInPath) src.add ("PRE-RENDER is on and its cache is not ready");
            else                         src.add ("the file player plays and outputs nothing (end of file / silent passage / decode)");
        }
        if (f.sourceMode == 1 && a.in[stAudio] <= floor)
            src.add ("live input is silent (input pair / cable / interface gain)");
        return src.isEmpty() ? juce::String ("nothing was asked to sound") : src.joinIntoString ("; ");
    }

    void incident (const juce::String& kind, const juce::String& what, const Frame& f, juce::uint32 sinceMs = 0)
    {
        ++incidents;
        Acc a;
        for (const auto& w : window)
            if (sinceMs == 0 || w.ms >= sinceMs) accumulate (a, w.f, w.prev);
        lastIncident = kind + ": " + what;
        write ("!!! " + lastIncident);
        write ("  window: " + statusLine (a, f));
        write ("  diagnosis: " + diagnose (a, f));
        writeDetail();
    }

    void writeDetail()
    {
        if (describe == nullptr) return;
        juce::StringArray lines;
        lines.addLines (describe());
        for (auto& l : lines) write ("  | " + l);
    }

    void logChanges (const Frame& f, bool first)
    {
        if (first)
        {
            write ("EVENT device " + f.devType + " / " + f.devName + (f.devOpen ? "" : " (not open)")
                   + " sr=" + juce::String (f.sampleRate, 0) + " buf=" + juce::String (f.buffer)
                   + " out=" + juce::String (f.outChans) + " in=" + juce::String (f.inChans));
            return;
        }
        if (f.devType != prev.devType || f.devName != prev.devName || f.devOpen != prev.devOpen
            || f.sampleRate != prev.sampleRate || f.buffer != prev.buffer || f.outChans != prev.outChans || f.inChans != prev.inChans)
            write ("EVENT device -> " + f.devType + " / " + f.devName + (f.devOpen ? "" : " (not open)")
                   + " sr=" + juce::String (f.sampleRate, 0) + " buf=" + juce::String (f.buffer)
                   + " out=" + juce::String (f.outChans) + " in=" + juce::String (f.inChans));
        if (f.devPlaying != prev.devPlaying)
            write (juce::String ("EVENT device ") + (f.devPlaying ? "playing" : "NOT playing"));
        if (f.sourceMode != prev.sourceMode)
            write (juce::String ("EVENT source -> ") + modeName (f.sourceMode));
        for (int st = 0; st < numStages; ++st)
            if (f.stSounding[st] != prev.stSounding[st] || f.stPresent[st] != prev.stPresent[st])
                write (juce::String ("EVENT strip ") + stageName (st) + (f.stPresent[st] ? "" : " (empty)")
                       + (f.stSounding[st] ? " sounding" : " silenced"));
    }

    void write (const juce::String& text)
    {
        const auto line = juce::Time::getCurrentTime().formatted ("%H:%M:%S") + "."
                          + juce::String (juce::Time::getCurrentTime().getMilliseconds()).paddedLeft ('0', 3) + " " + text;
        const juce::ScopedLock sl (fileLock);
        recent.add (line);
        if (recent.size() > 400) recent.removeRange (0, recent.size() - 400);
        if (stream != nullptr) { stream->writeText (line + "\r\n", false, false, nullptr); stream->flush(); }
    }

public:
    juce::StringArray getRecent (int n) const
    {
        const juce::ScopedLock sl (fileLock);
        juce::StringArray r;
        for (int i = juce::jmax (0, recent.size() - n); i < recent.size(); ++i) r.add (recent[i]);
        return r;
    }

private:
    // Watchdog: the message thread feeds `heartbeatMs` every tick.
    void run() override
    {
        bool reported = false;
        while (! threadShouldExit())
        {
            wait (500);
            if (! armed.load()) continue;      // the constructor (plug-in cache, device open) may take seconds: not a hang
            const auto now = juce::Time::getMillisecondCounter();
            const auto beat = heartbeatMs.load();
            if (now - beat > 2000 && ! reported) { reported = true; write ("!!! HANG the message thread has not ticked for " + juce::String (now - beat) + " ms (UI / plug-in editor / load blocking)"); }
            else if (now - beat <= 2000 && reported) { reported = false; write ("RECOVER message thread ticking again"); }
        }
    }

    static void pruneOld (const juce::File& dir, int keep)
    {
        auto files = dir.findChildFiles (juce::File::findFiles, false, "diag_*.txt");
        struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compare (b.getFileName()); } };
        ByName cmp;
        files.sort (cmp);
        for (int i = 0; i < files.size() - keep; ++i) files[i].deleteFile();
    }

    juce::File logFile;
    juce::CriticalSection fileLock;
    std::unique_ptr<juce::FileOutputStream> stream;
    juce::StringArray recent;
    std::atomic<juce::uint32> heartbeatMs { 0 };
    std::atomic<bool> armed { false };

    Frame prev;
    bool hasPrev = false;
    Acc sec;
    std::deque<WindowFrame> window;
    juce::uint32 secondStartMs = 0, lastCbAdvanceMs = 0, lastWrittenStatusMs = 0;
    juce::uint32 notesPendingMs = 0, fileSilentMs = 0, lastNoteMs = 0;
    bool stallReported = false, silentReported = false;
    int incidents = 0;
    juce::String lastIncident, summary, lastStatusBody;
};
