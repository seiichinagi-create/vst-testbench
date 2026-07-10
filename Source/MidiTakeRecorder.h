#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <cstring>

//==============================================================================
// Dashcam-style MIDI capture: no arm/stop workflow, noodling is never lost.
// Every message arriving on the MIDI inputs is recorded; a take opens on the
// first note-on and auto-saves as a .mid into the MIDI folder once the input
// has been quiet for a while with no key held. Tiny takes (a couple of
// accidental touches) are discarded.
//
// handleMessage() runs on the MIDI system thread; poll()/closeAndSave() on
// the message thread. One lock guards the sequence - MIDI events are rare
// (hundreds/s worst case), so contention is negligible.
//==============================================================================
class MidiTakeRecorder
{
public:
    MidiTakeRecorder() = default;   // NON_COPYABLE macro suppresses the implicit one

    static constexpr double idleCloseSeconds = 10.0;  // quiet gap that ends a take
    static constexpr int    minNoteOns       = 3;     // fewer = accidental touch, discarded

    void setDirectory (const juce::File& dir)  { directory = dir; }
    void setEnabled (bool b)                   { enabled.store (b); }   // off mid-take: closeAndSave() keeps it
    bool isEnabled() const                     { return enabled.load(); }

    bool isTakeOpen() const                    { const juce::ScopedLock sl (lock); return takeOpen; }
    int  getNoteCount() const                  { const juce::ScopedLock sl (lock); return noteOns; }

    // MIDI-thread entry.
    void handleMessage (const juce::MidiMessage& m)
    {
        if (! enabled.load())
            return;
        const double now = juce::Time::getMillisecondCounterHiRes();
        const juce::ScopedLock sl (lock);

        if (! takeOpen)
        {
            if (! m.isNoteOn())
                return;                        // a take starts with a note
            takeOpen     = true;
            takeStartMs  = now;
            takeWallTime = juce::Time::getCurrentTime();
            seq.clear();
            noteOns   = 0;
            heldCount = 0;
            std::memset (held, 0, sizeof (held));
        }

        auto stamped = m;
        stamped.setTimeStamp ((now - takeStartMs) / 1000.0);   // seconds
        seq.addEvent (stamped);
        lastMsgMs = now;

        if (m.isNoteOn() || m.isNoteOff())
        {
            const int ch = m.getChannel() - 1, note = m.getNoteNumber();
            if (ch >= 0 && ch < 16 && note >= 0 && note < 128)
            {
                if (m.isNoteOn())
                {
                    ++noteOns;
                    if (! held[ch][note]) { held[ch][note] = true; ++heldCount; }
                }
                else if (held[ch][note]) { held[ch][note] = false; --heldCount; }
            }
        }
    }

    // Message-thread poll (owner's timer). Returns the saved file when a take
    // closed on this call, File{} when nothing closed or the take was discarded.
    juce::File poll()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const juce::ScopedLock sl (lock);
        if (! takeOpen || heldCount > 0 || now - lastMsgMs < idleCloseSeconds * 1000.0)
            return {};
        return closeLocked();
    }

    // Force-close (shutdown / REC toggled off): keeps what was played.
    juce::File closeAndSave()
    {
        const juce::ScopedLock sl (lock);
        if (! takeOpen)
            return {};
        return closeLocked();
    }

private:
    juce::File closeLocked()
    {
        takeOpen = false;
        if (noteOns < minNoteOns || ! directory.isDirectory())
        {
            seq.clear();
            return {};
        }

        // 120 bpm grid: seconds -> ticks at 960 tpq (0.5 s per quarter note).
        juce::MidiMessageSequence ticks;
        ticks.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0.0);
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto msg = seq.getEventPointer (i)->message;
            msg.setTimeStamp (msg.getTimeStamp() * 1920.0);
            ticks.addEvent (msg);
        }
        ticks.addEvent (juce::MidiMessage::endOfTrack(), ticks.getEndTime() + 960.0);

        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        mf.addTrack (ticks);

        auto file = directory.getChildFile ("rec_" + takeWallTime.formatted ("%Y%m%d_%H%M%S") + ".mid")
                        .getNonexistentSibling();
        juce::FileOutputStream out (file);
        const bool ok = out.openedOk() && mf.writeTo (out);
        out.flush();
        seq.clear();
        return ok ? file : juce::File{};
    }

    juce::File directory;
    std::atomic<bool> enabled { true };
    mutable juce::CriticalSection lock;
    juce::MidiMessageSequence seq;
    bool       takeOpen    = false;
    double     takeStartMs = 0.0, lastMsgMs = 0.0;
    juce::Time takeWallTime;
    int        noteOns = 0, heldCount = 0;
    bool       held[16][128] = {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiTakeRecorder)
};
