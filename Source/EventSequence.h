#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "MpeSupport.h"

//==============================================================================
// JSON events -> a MIDI sequence (seconds). Shared by the control server and the rig worker process.
// {"t":0.5,"type":"note_on","ch":2,"note":60,"vel":100,"dur":1.0}
// types: note_on note_off pitch_bend pressure poly_pressure cc slide program all_off
//==============================================================================
inline juce::MidiMessageSequence sequenceFromEvents (const juce::var& events, juce::String& error)
{
    juce::MidiMessageSequence seq;
    auto* arr = events.getArray();
    if (arr == nullptr)
    {
        error = "\"events\" must be an array";
        return seq;
    }

    for (const auto& e : *arr)
    {
        const auto type = e.getProperty ("type", {}).toString().toLowerCase();
        const double t  = e.hasProperty ("t") ? (double) e["t"] : 0.0;
        const int ch    = juce::jlimit (1, 16, (int) e.getProperty ("ch", 1));
        const int note  = juce::jlimit (0, 127, (int) e.getProperty ("note", 60));
        const int vel   = juce::jlimit (0, 127, (int) e.getProperty ("vel", 100));

        auto add = [&seq, t] (juce::MidiMessage m, double at)
        {
            m.setTimeStamp (at);
            seq.addEvent (m);
        };

        if (type == "note_on")
        {
            add (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), t);
            if (e.hasProperty ("dur"))
                add (juce::MidiMessage::noteOff (ch, note, (juce::uint8) 0), t + juce::jmax (0.0, (double) e["dur"]));
        }
        else if (type == "note_off")
            add (juce::MidiMessage::noteOff (ch, note, (juce::uint8) 0), t);
        else if (type == "pitch_bend")
        {
            // "bend": -1..1 (full range), or "value": raw 0..16383 (8192 = centre)
            const int raw = e.hasProperty ("bend")
                ? juce::jlimit (0, 16383, 8192 + juce::roundToInt ((double) e["bend"] * 8191.0))
                : juce::jlimit (0, 16383, (int) e.getProperty ("value", 8192));
            add (juce::MidiMessage::pitchWheel (ch, raw), t);
        }
        else if (type == "pressure")
            add (juce::MidiMessage::channelPressureChange (ch, juce::jlimit (0, 127, (int) e.getProperty ("value", 0))), t);
        else if (type == "poly_pressure")
            add (juce::MidiMessage::aftertouchChange (ch, note, juce::jlimit (0, 127, (int) e.getProperty ("value", 0))), t);
        else if (type == "cc" || type == "slide")
        {
            const int cc = type == "slide" ? 74 : juce::jlimit (0, 127, (int) e.getProperty ("cc", 0));
            add (juce::MidiMessage::controllerEvent (ch, cc, juce::jlimit (0, 127, (int) e.getProperty ("value", 0))), t);
        }
        else if (type == "program")
            add (juce::MidiMessage::programChange (ch, juce::jlimit (0, 127, (int) e.getProperty ("value", 0))), t);
        else if (type == "all_off")
        {
            for (int c = 1; c <= 16; ++c)
            {
                add (juce::MidiMessage::allNotesOff (c), t);
                add (juce::MidiMessage::allSoundOff (c), t);
            }
        }
        else
        {
            error = "unknown event type \"" + type + "\"";
            return {};
        }
    }

    seq.sort();
    mpe::noteOnsLast (seq);
    return seq;
}
