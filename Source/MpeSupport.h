#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <set>

//==============================================================================
// MPE helpers shared by the live path, the MIDI-file bounce and the control
// server. The bench never interprets MPE itself: the instrument does. What the
// host must guarantee is (1) channels survive every hop untouched, (2) the MPE
// zone / pitch-bend-range setup reaches the instrument before the first note,
// (3) at equal timestamps the per-channel expression (bend / pressure / slide)
// is delivered before the note-on it belongs to.
//==============================================================================
namespace mpe
{
    struct Config
    {
        bool enabled   = false;   // add a lower-zone setup when the stream has none
        int  members   = 15;      // member channels 2..16
        int  memberPB  = 48;      // semitones, per-note pitch bend range
        int  masterPB  = 2;       // semitones, master channel range
    };

    // RPN 6 (MPE configuration) on a manager channel: CC101=0, CC100=6, CC6=n.
    inline bool isZoneSetupMessage (const juce::MidiMessage& m)
    {
        return m.isController() && m.getControllerNumber() == 6
            && (m.getChannel() == 1 || m.getChannel() == 16);
    }

    // True when the sequence already configures an MPE zone (RPN 6 data entry
    // on channel 1 or 16 right after CC101=0 / CC100=6).
    inline bool hasZoneSetup (const juce::MidiMessageSequence& seq)
    {
        int rpnMsb[17] = {}, rpnLsb[17] = {};
        for (int i = 0; i < 17; ++i) rpnMsb[i] = rpnLsb[i] = 127;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (! m.isController()) continue;
            const int ch = m.getChannel();
            if (m.getControllerNumber() == 101) rpnMsb[ch] = m.getControllerValue();
            else if (m.getControllerNumber() == 100) rpnLsb[ch] = m.getControllerValue();
            else if (m.getControllerNumber() == 6 && (ch == 1 || ch == 16)
                     && rpnMsb[ch] == 0 && rpnLsb[ch] == 6)
                return true;
        }
        return false;
    }

    // Note-carrying channels, 1-based. MPE-looking streams spread notes over
    // member channels 2..16 (or 1..15) instead of using one channel.
    inline std::set<int> noteChannels (const juce::MidiMessageSequence& seq)
    {
        std::set<int> chans;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isNoteOn()) chans.insert (m.getChannel());
        }
        return chans;
    }

    inline bool hasPitchBend (const juce::MidiMessageSequence& seq)
    {
        for (int i = 0; i < seq.getNumEvents(); ++i)
            if (seq.getEventPointer (i)->message.isPitchWheel())
                return true;
        return false;
    }

    inline bool looksMpe (const juce::MidiMessageSequence& seq)
    {
        return noteChannels (seq).size() >= 2 && hasZoneSetup (seq);
    }

    // Setup messages for the lower zone, in the order an instrument expects.
    inline juce::Array<juce::MidiMessage> zoneSetup (const Config& c)
    {
        juce::Array<juce::MidiMessage> out;
        const auto buf = juce::MPEMessages::setLowerZone (juce::jlimit (0, 15, c.members),
                                                          juce::jlimit (0, 96, c.memberPB),
                                                          juce::jlimit (0, 96, c.masterPB));
        for (const auto meta : buf)
            out.add (meta.getMessage());
        return out;
    }

    // Prepend the setup at t=0 in front of everything else (sequence order is
    // kept by the stable sort the caller runs afterwards).
    inline void prependZoneSetup (juce::MidiMessageSequence& seq, const Config& c)
    {
        juce::MidiMessageSequence out;
        for (auto m : zoneSetup (c))
        {
            m.setTimeStamp (0.0);
            out.addEvent (m);
        }
        out.addSequence (seq, 0.0);
        seq = std::move (out);
    }

    // Within one timestamp, deliver note-ons after every other event. Merged
    // tracks (or a controller that writes the note first) otherwise let a
    // note start with last note's bend / slide still applied on its channel.
    inline void noteOnsLast (juce::MidiMessageSequence& seq)
    {
        juce::MidiMessageSequence out;
        int i = 0;
        const int n = seq.getNumEvents();
        while (i < n)
        {
            int j = i;
            const double t = seq.getEventPointer (i)->message.getTimeStamp();
            while (j < n && seq.getEventPointer (j)->message.getTimeStamp() == t) ++j;
            for (int k = i; k < j; ++k)
                if (! seq.getEventPointer (k)->message.isNoteOn()) out.addEvent (seq.getEventPointer (k)->message);
            for (int k = i; k < j; ++k)
                if (seq.getEventPointer (k)->message.isNoteOn()) out.addEvent (seq.getEventPointer (k)->message);
            i = j;
        }
        seq = std::move (out);
    }

    // ---------------------------------------------------------------------
    // TOP-BEND: a one-channel file whose pitch bend is meant for the highest
    // sounding note only. One channel cannot do that (bend hits every note), so
    // each note is moved to its own MPE member channel and the source bend is
    // routed to the member channel of the current top note.
    //   * a note always starts with its channel bend at centre;
    //   * when a higher note-on takes over as top, the old top's bend returns to
    //     centre (the bend does NOT move to the new top);
    //   * source CC / program / channel pressure go to the master channel (1),
    //     which an MPE zone applies to every member;
    //   * the source bend range (RPN 0, default 2 st) is rescaled to the member
    //     range, and the source RPN messages are dropped.
    // Input: sorted, times in seconds. Notes of each source channel are handled
    // independently; members come from one shared pool (channels 2..members+1).
    struct TopBendResult { juce::MidiMessageSequence seq; int notes = 0; int bends = 0; int steals = 0; };

    inline TopBendResult topBendSplit (const juce::MidiMessageSequence& in, const Config& cfg)
    {
        TopBendResult res;
        const int first = 2, last = juce::jlimit (2, 16, 1 + cfg.members);
        const int poolSize = last - first + 1;

        struct Member { bool busy = false; int srcCh = 0, note = 0; double freedAt = -1.0; double startedAt = 0.0; };
        Member pool[17];
        struct Src { int bendValue = 8192; double rangeSemi = 2.0; int rpnMsb = 127, rpnLsb = 127, dataMsb = 2, dataLsb = 0; };
        Src src[17];

        auto emit = [&res] (juce::MidiMessage m, double t) { m.setTimeStamp (t); res.seq.addEvent (m); };
        const int centre = 8192;

        auto rawFor = [&cfg] (const Src& s)
        {
            const double norm = ((double) s.bendValue - 8192.0) / 8192.0;           // -1..1 of the source range
            const double v = norm * s.rangeSemi / (double) juce::jmax (1, cfg.memberPB);
            return juce::jlimit (0, 16383, 8192 + juce::roundToInt (v * 8192.0));
        };

        auto topOf = [&pool, first, last] (int srcCh) -> int   // member channel carrying the highest note, 0 = none
        {
            int best = 0, bestNote = -1;
            for (int c = first; c <= last; ++c)
                if (pool[c].busy && pool[c].srcCh == srcCh && pool[c].note > bestNote)
                    { bestNote = pool[c].note; best = c; }
            return best;
        };

        for (int i = 0; i < in.getNumEvents(); ++i)
        {
            const auto& m = in.getEventPointer (i)->message;
            const double t = m.getTimeStamp();

            if (m.isMetaEvent() || m.getChannel() == 0)
            {
                emit (m, t);
                continue;
            }
            const int sc = m.getChannel();

            if (m.isNoteOn())
            {
                const int note = m.getNoteNumber();
                // same key still sounding: release it first
                for (int c = first; c <= last; ++c)
                    if (pool[c].busy && pool[c].srcCh == sc && pool[c].note == note)
                    {
                        emit (juce::MidiMessage::noteOff (c, note, (juce::uint8) 0), t);
                        pool[c].busy = false; pool[c].freedAt = t;
                    }

                // free member, least recently released first; else steal the oldest note
                int pick = 0;
                for (int c = first; c <= last; ++c)
                    if (! pool[c].busy && (pick == 0 || pool[c].freedAt < pool[pick].freedAt))
                        pick = c;
                if (pick == 0)
                {
                    for (int c = first; c <= last; ++c)
                        if (pick == 0 || pool[c].startedAt < pool[pick].startedAt) pick = c;
                    emit (juce::MidiMessage::noteOff (pick, pool[pick].note, (juce::uint8) 0), t);
                    ++res.steals;
                }

                const int oldTop = topOf (sc);
                emit (juce::MidiMessage::pitchWheel (pick, centre), t);   // note starts un-bent
                emit (juce::MidiMessage::noteOn (pick, note, m.getVelocity()), t);
                pool[pick] = { true, sc, note, -1.0, t };
                ++res.notes;

                if (oldTop != 0 && pool[oldTop].note < note)
                    emit (juce::MidiMessage::pitchWheel (oldTop, centre), t);   // top changed: old top back to centre
                continue;
            }

            if (m.isNoteOff())
            {
                const int note = m.getNoteNumber();
                for (int c = first; c <= last; ++c)
                    if (pool[c].busy && pool[c].srcCh == sc && pool[c].note == note)
                    {
                        emit (juce::MidiMessage::noteOff (c, note, m.getVelocity()), t);
                        pool[c].busy = false; pool[c].freedAt = t;
                        break;
                    }
                continue;
            }

            if (m.isPitchWheel())
            {
                src[sc].bendValue = m.getPitchWheelValue();
                if (const int top = topOf (sc))
                {
                    emit (juce::MidiMessage::pitchWheel (top, rawFor (src[sc])), t);
                    ++res.bends;
                }
                continue;
            }

            if (m.isController())
            {
                const int cc = m.getControllerNumber(), v = m.getControllerValue();
                if (cc == 101) { src[sc].rpnMsb = v; continue; }
                if (cc == 100) { src[sc].rpnLsb = v; continue; }
                if (cc == 6 || cc == 38)
                {
                    if (src[sc].rpnMsb == 0 && src[sc].rpnLsb == 0)   // pitch bend range
                    {
                        if (cc == 6)  src[sc].dataMsb = v; else src[sc].dataLsb = v;
                        src[sc].rangeSemi = src[sc].dataMsb + src[sc].dataLsb / 100.0;
                    }
                    continue;   // RPN data never reaches the members
                }
                emit (juce::MidiMessage::controllerEvent (1, cc, v), t);
                continue;
            }

            if (m.isChannelPressure() || m.isProgramChange())
            {
                auto copy = m;
                copy.setChannel (1);
                emit (copy, t);
                continue;
            }
            // poly pressure: follow its note to the member channel
            if (m.isAftertouch())
            {
                for (int c = first; c <= last; ++c)
                    if (pool[c].busy && pool[c].srcCh == sc && pool[c].note == m.getNoteNumber())
                    {
                        emit (juce::MidiMessage::aftertouchChange (c, m.getNoteNumber(), m.getAfterTouchValue()), t);
                        break;
                    }
                continue;
            }
            emit (m, t);
        }
        juce::ignoreUnused (poolSize);
        return res;
    }
}
