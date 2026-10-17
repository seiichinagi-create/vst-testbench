#include "MainComponent.h"

//==============================================================================
// Sound diagnostics, the host side (SoundDiag.h does the logging and the reasoning). Each timer tick the bench hands
// SoundDiag one frame: device state, what the audio thread saw (TransportPlayer::probe), the peaks on both sides of
// every strip and the note-ons at each instrument's MIDI filter.
//==============================================================================
void MainComponent::startSoundDiag()
{
    soundDiag.describe = [this] { return describeForDiag(); };
    soundDiag.start (appDir().getChildFile ("diag"));
}

void MainComponent::tickSoundDiag()
{
    SoundDiag::Frame f;
    f.ms = juce::Time::getMillisecondCounter();

    f.devType = deviceManager.getCurrentAudioDeviceType();
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        f.devName    = dev->getName();
        f.devOpen    = dev->isOpen();
        f.devPlaying = dev->isPlaying();
        f.sampleRate = dev->getCurrentSampleRate();
        f.buffer     = dev->getCurrentBufferSizeSamples();
        f.outChans   = dev->getActiveOutputChannels().countNumberOfSetBits();
        f.inChans    = dev->getActiveInputChannels().countNumberOfSetBits();
        f.xruns      = dev->getXRunCount();
    }
    f.cpu = deviceManager.getCpuUsage();

    auto& p = player.probe;
    f.callbacks    = p.callbacks.load();
    f.devPeak      = p.takeOutPeak();
    f.maxBlockMs   = p.takeMaxBlockMs();
    f.overBudget   = p.overBudget.load();
    f.nonFinite    = p.nonFinite.load();
    f.midiHw       = p.midiHw.load();
    f.midiHwNotes  = p.midiHwNotes.load();
    f.midiInj      = p.midiInj.load();
    f.midiInjNotes = p.midiInjNotes.load();
    f.starts       = p.starts.load();
    f.stops        = p.stops.load();
    f.errors       = p.errors.load();
    if (f.errors > 0) f.lastError = p.getLastError();

    TrackStrip* st[SoundDiag::numStages] = { strips[trkAudio], strips[trkInst1], strips[trkInst2], strips[trkInst3],
                                             busReturn[0], busReturn[1], masterStrip };
    for (int s = 0; s < SoundDiag::numStages; ++s)
        if (st[s] != nullptr)
        {
            f.stIn[s]       = st[s]->takeDiagIn();
            f.stOut[s]      = st[s]->takeDiagOut();
            f.stSounding[s] = st[s]->isSounding();
            f.stModeOff[s]  = st[s]->isModeSilenced() && st[s]->isAudible();
        }
    f.stPresent[SoundDiag::stAudio]  = true;    // the file player or the live input
    f.stPresent[SoundDiag::stInst1]  = instrumentNode != nullptr;
    f.stPresent[SoundDiag::stInst2]  = extra (trkInst2).node != nullptr;
    f.stPresent[SoundDiag::stInst3]  = extra (trkInst3).node != nullptr;
    f.stPresent[SoundDiag::stRet1]   = busSlot[0].node != nullptr;
    f.stPresent[SoundDiag::stRet2]   = busSlot[1].node != nullptr;
    f.stPresent[SoundDiag::stMaster] = true;
    for (int t = 0; t < 3; ++t)
        if (auto* mf = midiFilters[trkInst1 + t])
        {
            f.instSeen[t]   = mf->getNotesSeen();
            f.instPassed[t] = mf->getNotesPassed();
        }

    f.sourceMode       = currentSourceMode();
    f.fileLoaded       = filePlayer != nullptr && filePlayer->hasFile();
    f.filePlaying      = filePlayer != nullptr && filePlayer->transport.isPlaying();
    f.preRender        = preRenderActive();
    f.cacheInPath      = cacheInPath();
    f.transportPlaying = liveTransport.isPlaying();
    f.connections      = (int) graph.getConnections().size();

    soundDiag.push (f);

    diagLabel.setText (soundDiag.getSummary(), juce::dontSendNotification);
    diagLabel.setColour (juce::Label::textColourId, soundDiag.getIncidents() > 0 ? juce::Colour (0xffe07a5f) : juce::Colours::grey);
    diagLabel.setTooltip (soundDiag.getLastIncident());
}

juce::String MainComponent::describeForDiag() const
{
    juce::StringArray out;

    {
        juce::String d = "device: " + deviceManager.getCurrentAudioDeviceType();
        if (auto* dev = deviceManager.getCurrentAudioDevice())
            d << " / " << dev->getName() << (dev->isOpen() ? "" : " (closed)") << (dev->isPlaying() ? " playing" : " NOT playing")
              << " sr=" << juce::String (dev->getCurrentSampleRate(), 0) << " buf=" << dev->getCurrentBufferSizeSamples()
              << " out=" << dev->getActiveOutputChannels().toString (2) << " in=" << dev->getActiveInputChannels().toString (2)
              << " latency(out)=" << dev->getOutputLatencyInSamples() << " xruns=" << dev->getXRunCount();
        else
            d << " / (none)";
        out.add (d);
    }
    {
        juce::StringArray ins;
        for (const auto& m : juce::MidiInput::getAvailableDevices())
            ins.add (m.name + (deviceManager.isMidiInputDeviceEnabled (m.identifier) ? " [on]" : " [off]"));
        out.add ("MIDI inputs: " + (ins.isEmpty() ? juce::String ("(none)") : ins.joinIntoString (", ")));
    }
    out.add ("graph: " + juce::String (graph.getNumNodes()) + " nodes, " + juce::String ((int) graph.getConnections().size())
             + " connections" + (graph.isSuspended() ? ", SUSPENDED" : "") + ", pre-render " + (preRenderActive() ? "on" : "off")
             + (cacheInPath() ? " (cache plays)" : ""));

    auto node = [&out] (const juce::String& role, const Graph::Node::Ptr& n)
    {
        if (n == nullptr) return;
        auto* proc = n->getProcessor();
        out.add ("  " + role + ": " + proc->getName()
                 + " in=" + juce::String (proc->getTotalNumInputChannels()) + " out=" + juce::String (proc->getTotalNumOutputChannels())
                 + (n->isBypassed() ? " BYPASSED" : "") + (proc->isSuspended() ? " SUSPENDED" : "")
                 + " latency=" + juce::String (proc->getLatencySamples())
                 + " sr=" + juce::String (proc->getSampleRate(), 0) + " blk=" + juce::String (proc->getBlockSize()));
    };
    node ("master FX", effectNode);
    node ("INST1", instrumentNode);
    node ("INST2", extra (trkInst2).node);
    node ("INST3", extra (trkInst3).node);
    for (int t = 0; t < numTracks; ++t) node ("insert " + juce::String (t), insertSlot[t].node);
    node ("send1 FX", busSlot[0].node);
    node ("send2 FX", busSlot[1].node);
    if (filePlayer != nullptr)
        out.add ("  file: " + (filePlayer->hasFile() ? currentPlayableFile.getFileName() : juce::String ("(none)"))
                 + (filePlayer->transport.isPlaying() ? " playing at " : " stopped at ")
                 + juce::String (filePlayer->transport.getCurrentPosition(), 2) + " s");

    auto strip = [&out] (const juce::String& name, const TrackStrip* s, const MidiChannelFilter* mf)
    {
        if (s == nullptr) return;
        out.add ("  strip " + name + ": " + juce::String (s->getGainDb(), 1) + " dB bal " + juce::String (s->getBalance(), 2)
                 + (s->isMuted() ? " MUTE" : "") + (s->isSolo() ? " SOLO" : "") + (s->isAudible() || s->isMuted() ? "" : " solo-silenced")
                 + (s->isModeSilenced() ? " mode-silenced" : "")
                 + (mf != nullptr ? " midi-mask=0x" + juce::String::toHexString ((int) mf->getMask()) : juce::String()));
    };
    strip ("AUDIO", strips[trkAudio], nullptr);
    for (int t = trkInst1; t < numTracks; ++t) strip ("INST" + juce::String (t), strips[t], midiFilters[t]);
    strip ("RET1", busReturn[0], nullptr);
    strip ("RET2", busReturn[1], nullptr);
    strip ("MASTER", masterStrip, nullptr);

    auto nameOf = [this] (Graph::NodeID id)
    {
        auto n = graph.getNodeForId (id);
        return (n != nullptr ? n->getProcessor()->getName() : juce::String ("?")) + "#" + juce::String ((int) id.uid);
    };
    auto pin = [] (const Graph::NodeAndChannel& nc) { return nc.isMIDI() ? juce::String ("midi") : juce::String (nc.channelIndex); };
    juce::StringArray wires;
    for (auto& c : graph.getConnections())
        wires.add (nameOf (c.source.nodeID) + ":" + pin (c.source) + "->" + nameOf (c.destination.nodeID) + ":" + pin (c.destination));
    wires.sort (false);
    out.add ("wiring: " + wires.joinIntoString ("  "));
    return out.joinIntoString ("\n");
}

// diag      -> { log, incidents, last_incident, summary, lines: [the last n (default 60) log lines] }
// diag_mark -> writes a MARK (note = "...") first, then the same
juce::var MainComponent::diagControl (const juce::var& req)
{
    if (req["cmd"].toString().equalsIgnoreCase ("diag_mark"))
        soundDiag.mark ("control: " + (req.hasProperty ("note") ? req["note"].toString() : juce::String ("no sound")));
    auto* o = new juce::DynamicObject();
    o->setProperty ("log", soundDiag.getLogFile().getFullPathName());
    o->setProperty ("incidents", soundDiag.getIncidents());
    o->setProperty ("last_incident", soundDiag.getLastIncident());
    o->setProperty ("summary", soundDiag.getSummary());
    juce::Array<juce::var> lines;
    for (auto& l : soundDiag.getRecent (req.hasProperty ("n") ? (int) req["n"] : 60)) lines.add (l);
    o->setProperty ("lines", lines);
    return juce::var (o);
}
