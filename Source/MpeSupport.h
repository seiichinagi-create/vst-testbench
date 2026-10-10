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
}
