#include "MainComponent.h"

//==============================================================================
// MIDI input per INST track. Every hardware message comes through handleIncomingMidiMessage (which knows the device)
// and every injected one through injectMidi; both land here. A track takes a message when its input is "all" or that
// device (injected messages reach every track); its MidiChannelFilter then applies the channel mask, in the audio
// thread, on the track's own queue. The master FX (MIDI-controlled effects) gets everything through the player.
//==============================================================================
void MainComponent::routeMidi (const juce::MidiMessage& m, juce::MidiInput* source)
{
    auto& p = player.probe;
    const bool note = m.isNoteOn();
    const int ch = m.getChannel();
    if (source != nullptr) { p.midiHw.fetch_add (1); if (note) p.midiHwNotes.fetch_add (1); }
    else                   { p.midiInj.fetch_add (1); if (note) p.midiInjNotes.fetch_add (1); }
    if (note && ch >= 1) p.noteChannels.fetch_or (1u << (ch - 1));

    player.getMidiMessageCollector().addMessageToQueue (m);

    MidiLogEntry e;
    e.time = m.getTimeStamp();
    e.source = source != nullptr ? source->getName() : juce::String ("(injected)");
    e.msg = m;
    const juce::String srcId = source != nullptr ? source->getIdentifier() : juce::String();
    for (int t = trkInst1; t < numTracks; ++t)
    {
        bool deviceOk;
        {
            const juce::SpinLock::ScopedLockType sl (midiRouteLock);
            deviceOk = source == nullptr || trackMidiInId[t].isEmpty() || trackMidiInId[t] == srcId;
        }
        if (! deviceOk)
            continue;
        midiFilters[t]->feed (m);
        if (ch == 0 || (midiFilters[t]->getMask() & (1u << (ch - 1))) != 0)
        {
            const auto bit = (juce::uint8) (1 << (t - trkInst1));
            e.tracks |= bit;
            const auto* proc = t == trkInst1 ? instrumentProc.load() : extraProc[t - trkInst2].load();
            if (proc == nullptr) e.emptyTracks |= bit;
        }
    }
    midiLog.add (std::move (e));   // clock / active sensing too: the monitor hides them unless asked
}

void MainComponent::setTrackMidiInput (int track, const juce::String& want)
{
    if (track < trkInst1 || track >= numTracks)
        return;
    juce::String id, name;
    if (want.isNotEmpty() && ! want.equalsIgnoreCase ("all"))
    {
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.identifier == want || d.name == want) { id = d.identifier; name = d.name; break; }
        if (id.isEmpty())
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                if (d.name.containsIgnoreCase (want)) { id = d.identifier; name = d.name; break; }
        if (id.isEmpty())
        {
            setStatus ("MIDI input \"" + want + "\" not found - INST " + juce::String (track) + " unchanged.");
            return;
        }
        if (! deviceManager.isMidiInputDeviceEnabled (id))
            deviceManager.setMidiInputDeviceEnabled (id, true);   // a chosen input must deliver
    }
    {
        const juce::SpinLock::ScopedLockType sl (midiRouteLock);
        trackMidiInId[track] = id;
        trackMidiInName[track] = name;
    }
    saveTracks();
    soundDiag.event ("INST" + juce::String (track) + " MIDI input -> " + trackMidiInputLabel (track));
    if (mixerPanel != nullptr) mixerPanel->refresh();
}

juce::String MainComponent::trackMidiInputLabel (int track) const
{
    juce::String id, name;
    {
        const juce::SpinLock::ScopedLockType sl (const_cast<juce::SpinLock&> (midiRouteLock));
        id = trackMidiInId[track];
        name = trackMidiInName[track];
    }
    if (id.isEmpty()) return "all";
    for (const auto& d : juce::MidiInput::getAvailableDevices())
        if (d.identifier == id)
            return d.name + (deviceManager.isMidiInputDeviceEnabled (id) ? "" : " (disabled)");
    return name + " (not connected)";
}

juce::StringArray MainComponent::midiRouteInfo() const
{
    juce::StringArray out;
    for (int t = trkInst1; t < numTracks; ++t)
    {
        const auto plugin = t == trkInst1 ? currentInstrumentName : extra (t).name;
        out.add ("INST " + juce::String (t) + " [" + (plugin.isEmpty() ? juce::String ("empty") : plugin) + "]: input "
                 + trackMidiInputLabel (t) + ", channels " + MixerPanel::formatChannels (midiFilters[t]->getMask()));
    }
    return out;
}

void MainComponent::showMidiMonitor()
{
    if (midiMonitorWindow == nullptr)
        midiMonitorWindow = std::make_unique<MidiMonitorWindow> (midiLog, [this] { return midiRouteInfo(); });
    midiMonitorWindow->setVisible (true);
    midiMonitorWindow->toFront (true);
}
