#include "MainComponent.h"
#include "EventSequence.h"

//==============================================================================
// AI control surface: every command the control server accepts. See
// docs/CONTROL.md for the protocol and tools/tb.py for the client. All of this
// runs on the message thread (ControlServer::dispatch).
//==============================================================================
namespace
{
    juce::var makeObj() { return juce::var (new juce::DynamicObject()); }

    void put (juce::var& o, const juce::Identifier& k, const juce::var& v)
    {
        o.getDynamicObject()->setProperty (k, v);
    }

    juce::var fail (const juce::String& msg)
    {
        auto o = makeObj();
        put (o, "error", msg);
        return o;
    }

    juce::String str (const juce::var& req, const char* key, const juce::String& def = {})
    {
        return req.hasProperty (key) ? req[key].toString() : def;
    }

    double num (const juce::var& req, const char* key, double def)
    {
        return req.hasProperty (key) ? (double) req[key] : def;
    }

    bool flag (const juce::var& req, const char* key, bool def)
    {
        return req.hasProperty (key) ? (bool) req[key] : def;
    }

    double toDb (double linear) { return linear > 1.0e-6 ? 20.0 * std::log10 (linear) : -120.0; }

    // A client may hand over a file name only; relative paths are relative to
    // the bench's data folder so an AI always has a writable place.
    juce::File resolvePath (const juce::String& s, const juce::File& base)
    {
        if (juce::File::isAbsolutePath (s))
            return juce::File (s);
        return base.getChildFile (s);
    }

    juce::String roleOf (const juce::var& req) { return str (req, "role", "fx").toLowerCase(); }
}

//==============================================================================
// midi_channels: [1, 2, 5] (1..16), "all" or "none"
juce::uint32 MainComponent::maskFromChannels (const juce::var& v)
{
    if (v.isString())
        return v.toString().equalsIgnoreCase ("all") ? 0xFFFFu : 0u;
    juce::uint32 m = 0;
    if (auto* a = v.getArray())
        for (const auto& c : *a)
            if ((int) c >= 1 && (int) c <= 16)
                m |= 1u << ((int) c - 1);
    return m;
}

// "audio" / "inst1".."inst3" -> 0..3, "master" -> -1, anything else -> -2
int MainComponent::trackIndexOf (const juce::String& s)
{
    if (s == "audio") return trkAudio;
    if (s == "inst1" || s == "inst") return trkInst1;
    if (s == "inst2") return trkInst2;
    if (s == "inst3") return trkInst3;
    if (s == "master") return -1;
    if (s == "send1") return tiBus1;
    if (s == "send2") return tiBus2;
    return -2;
}

// One track (or the master, t == -1) as the control API reports it.
juce::var MainComponent::trackInfo (int t) const
{
    auto o = makeObj();
    const bool bus = t >= tiBus1;
    const TrackStrip* strip = t == -1 ? masterStrip : bus ? busReturn[t - tiBus1] : strips[t];
    put (o, "track", t == -1 ? "master" : bus ? "send" + juce::String (t - tiBus1 + 1) : t == trkAudio ? "audio" : "inst" + juce::String (t));
    put (o, "gain_db", (double) strip->getGainDb());
    put (o, "balance", (double) strip->getBalance());
    put (o, "mute", strip->isMuted());
    if (t >= 0 && ! bus)
    {
        put (o, "solo", strip->isSolo());
        put (o, "silenced_by_mode", strip->isModeSilenced());
        put (o, "send1_db", (double) sendLevel[t][0]->getGainDb());
        put (o, "send2_db", (double) sendLevel[t][1]->getGainDb());
        put (o, "insert", insertSlot[t].name);
    }
    put (o, "sounding", (t < 0 || bus) ? ! strip->isMuted() : strip->isSounding());
    if (t == trkAudio)
        put (o, "source", currentSourceMode() == srcFile ? "file" : "live");
    if (t >= trkInst1 && ! bus)
    {
        const auto name = t == trkInst1 ? currentInstrumentName : extra (t).name;
        put (o, "plugin", name);
        put (o, "loaded", name.isNotEmpty());
        juce::Array<juce::var> ch;
        for (int c = 1; c <= 16; ++c)
            if ((midiFilters[t]->getMask() & (1u << (c - 1))) != 0)
                ch.add (c);
        put (o, "midi_channels", ch);
    }
    if (bus)
    {
        put (o, "plugin", busSlot[t - tiBus1].name);      // the FX of the bus; the strip is its return level
        put (o, "loaded", busSlot[t - tiBus1].node != nullptr);
    }
    if (t == -1)
    {
        put (o, "plugin", currentEffectName);   // the legacy FX is the master insert
        put (o, "loaded", effectNode != nullptr);
    }
    return o;
}

//==============================================================================
juce::AudioProcessor* MainComponent::processorForRole (const juce::String& role) const
{
    if (role == "fx")
        return effectNode != nullptr ? effectNode->getProcessor() : nullptr;
    if (role == "inst" || role == "instrument" || role == "inst1")
        return instrumentNode != nullptr ? instrumentNode->getProcessor() : nullptr;
    if (role == "inst2" || role == "inst3")
        return extra (trackFromRole (role)).node != nullptr ? extra (trackFromRole (role)).node->getProcessor() : nullptr;
    if (const int slot = slotFromRole (role); slot >= 0)
        return fxSlot (slot).node != nullptr ? fxSlot (slot).node->getProcessor() : nullptr;
    return nullptr;
}

//==============================================================================
// {"t":0.5,"type":"note_on","ch":2,"note":60,"vel":100,"dur":1.0}
// types: note_on note_off pitch_bend pressure poly_pressure cc program all_off
juce::MidiMessageSequence MainComponent::sequenceFromEvents (const juce::var& events, juce::String& error) const
{
    return ::sequenceFromEvents (events, error);
}

//==============================================================================
juce::var MainComponent::controlStatus() const
{
    auto o = makeObj();

    const int mode = currentSourceMode();
    put (o, "source", mode == srcLive ? "live" : mode == srcInstrument ? "inst" : "file");
    put (o, "status_text", statusLabel.getText());

    const bool busy = pendingLoads > 0 || bounceRunning() || rigWorker.isRunning() || araProbePending || offlineInstLoading || offlineFxLoading
                      || firstBouncePending || (preRenderActive() && ! mixBakeMode && renderEngine.isRendering())
                      || (preRenderActive() && ! mixBakeMode && fxStale.load())
                      || (mixBakeMode && preRenderActive() && (mixBakeRunning || mixBakeDirty.load() || ! mixBakeReady));
    put (o, "busy", busy);

    {
        auto fx = makeObj();
        put (fx, "loaded", effectNode != nullptr);
        put (fx, "name", currentEffectName);
        put (fx, "bypassed", bypassButton.getToggleState());
        put (o, "fx", fx);
    }
    {
        auto in = makeObj();
        put (in, "loaded", instrumentNode != nullptr);
        put (in, "name", currentInstrumentName);
        if (instrumentNode != nullptr)
            put (in, "outputs", instrumentNode->getProcessor()->getTotalNumOutputChannels());
        put (o, "inst", in);
    }
    {
        auto f = makeObj();
        put (f, "loaded", filePlayer->hasFile());
        put (f, "label", fileLabel.getText());
        put (f, "playing", filePlayer->transport.isPlaying());
        put (f, "position", filePlayer->transport.getCurrentPosition());
        put (f, "length", filePlayer->transport.getLengthInSeconds());
        put (f, "playable_wav", currentPlayableFile.getFullPathName());
        put (f, "loop", loopButton.getToggleState());
        put (o, "file", f);
    }
    {
        auto m = makeObj();
        put (m, "file", currentMidiFile.getFullPathName());
        put (m, "chain_active", midiChainActive());
        put (m, "bouncing", bounceRunning());
        put (m, "bounce_progress", (double) bounceEngine.progress.load());
        put (m, "status", midiStatusLabel.getText());
        put (m, "analysis", mpeNote);
        put (m, "live_playing", midiScheduler.isPlaying());
        put (m, "live_elapsed", midiScheduler.getElapsed());
        put (m, "live_duration", midiScheduler.getDuration());
        put (m, "take_recording", midiRecorder.isTakeOpen());
        put (o, "midi", m);
    }
    {
        auto p = makeObj();
        put (p, "enabled", mpeConfig.enabled);
        put (p, "members", mpeConfig.members);
        put (p, "member_pitchbend_semitones", mpeConfig.memberPB);
        put (p, "master_pitchbend_semitones", mpeConfig.masterPB);
        put (o, "mpe", p);
    }
    if (! rigResult.isVoid())
        put (o, "rig", rigResult);
    if (! araProbeResult.isVoid())
        put (o, "ara_probe", araProbeResult);
    put (o, "prerender", preRenderActive());
    put (o, "prerender_mode", ! preRenderActive() ? "off" : mixBakeMode ? "rig" : "engine");
    put (o, "gpu_fx", gpuFxButton.getToggleState());

    {
        auto d = makeObj();
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            put (d, "type", deviceManager.getCurrentAudioDeviceType());
            put (d, "name", dev->getName());
            put (d, "sample_rate", dev->getCurrentSampleRate());
            put (d, "buffer", dev->getCurrentBufferSizeSamples());
        }
        else
            put (d, "name", "(none)");
        put (o, "audio", d);
    }
    {
        // Levels since the previous status call, so polling gives a meter.
        auto l = makeObj();
        if (outputTap != nullptr)
        {
            put (l, "peak_db", toDb (outputTap->takePeak()));
            put (l, "rms_db", toDb (outputTap->takeRms()));
            put (l, "recording", outputTap->isRecording());
        }
        put (o, "level", l);
    }
    return o;
}

//==============================================================================
juce::var MainComponent::handleControl (const juce::var& req)
{
    const auto cmd = str (req, "cmd").toLowerCase();

    if (cmd == "ping")   { auto o = makeObj(); put (o, "pong", true); put (o, "app", "VST TestBench"); return o; }
    if (cmd == "status") return controlStatus();

    // The live graph's connections as sorted "node:channel -> node:channel" lines (MIDI = "midi"): what is wired to what.
    // For checking that a change to how the graph is built (docs/TRACKS.md, P1) leaves the routing exactly as it was.
    if (cmd == "graph_dump")
    {
        auto o = makeObj();
        auto nameOf = [this] (Graph::NodeID id)
        {
            auto n = graph.getNodeForId (id);
            return n != nullptr ? n->getProcessor()->getName() : juce::String ("?");
        };
        auto pin = [] (const Graph::NodeAndChannel& nc)
        {
            return nc.isMIDI() ? juce::String ("midi") : juce::String (nc.channelIndex);
        };
        juce::StringArray lines;
        for (auto& c : graph.getConnections())
            lines.add (nameOf (c.source.nodeID) + ":" + pin (c.source) + " -> " + nameOf (c.destination.nodeID) + ":" + pin (c.destination));
        lines.sort (false);
        juce::Array<juce::var> arr;
        for (auto& l : lines)
            arr.add (l);
        put (o, "connections", arr);
        return o;
    }

    //-- mixer: tracks (docs/TRACKS.md) ----------------------------------------
    // track = audio | inst1 | inst2 | inst3 | master;  gain_db, balance (-1..1), mute, solo, midi_channels (INST tracks)
    if (cmd == "track_set")
    {
        const int t = trackIndexOf (str (req, "track").toLowerCase());
        if (t == -2) return fail ("track must be audio, inst1, inst2, inst3, send1, send2 or master");
        TrackStrip* strip = t == -1 ? masterStrip : t >= tiBus1 ? busReturn[t - tiBus1] : strips[t];
        if (t >= 0 && t < numTracks)                                   // post-fader sends: -100 = off
        {
            if (req.hasProperty ("send1_db")) sendLevel[t][0]->setGainDb ((float) (double) req["send1_db"]);
            if (req.hasProperty ("send2_db")) sendLevel[t][1]->setGainDb ((float) (double) req["send2_db"]);
            if (req.hasProperty ("send1_db") || req.hasProperty ("send2_db")) markBakeDirty();
        }
        else if (req.hasProperty ("send1_db") || req.hasProperty ("send2_db"))
            return fail ("send1_db / send2_db apply to audio, inst1, inst2 and inst3");
        if (req.hasProperty ("gain_db")) strip->setGainDb ((float) (double) req["gain_db"]);
        if (req.hasProperty ("balance")) strip->setBalance ((float) (double) req["balance"]);
        if (req.hasProperty ("mute"))    strip->setMuted (flag (req, "mute", false));
        if (req.hasProperty ("solo"))
        {
            if (t < 0 || t >= tiBus1) return fail ("only the tracks have a solo");
            strip->setSolo (flag (req, "solo", false));
            updateSolo();
        }
        if (req.hasProperty ("midi_channels"))
        {
            if (t < trkInst1 || t >= numTracks) return fail ("midi_channels applies to inst1, inst2 and inst3");
            midiFilters[t]->setMask (maskFromChannels (req["midi_channels"]));
        }
        if (mixerPanel != nullptr) mixerPanel->refresh();
        saveTracks();
        markMixChanged (t);
        return trackInfo (t);
    }
    if (cmd == "track_status")
    {
        auto o = makeObj();
        juce::Array<juce::var> list;
        for (int t = trkAudio; t < numTracks; ++t) list.add (trackInfo (t));
        list.add (trackInfo (tiBus1));
        list.add (trackInfo (tiBus2));
        list.add (trackInfo (-1));
        put (o, "tracks", list);
        return o;
    }

    //-- plugins ---------------------------------------------------------------
    if (cmd == "list_plugins")
    {
        juce::Array<juce::var> list;
        for (const auto& t : knownPlugins.getTypes())
        {
            auto p = makeObj();
            put (p, "name", t.name);
            put (p, "kind", t.isInstrument ? "inst" : "fx");
            put (p, "file", t.fileOrIdentifier);
            put (p, "manufacturer", t.manufacturerName);
            list.add (p);
        }
        auto o = makeObj();
        put (o, "plugins", list);
        return o;
    }

    if (cmd == "load_plugin")
    {
        const auto role = roleOf (req);
        const int slotId = slotFromRole (role);
        if (role != "fx" && trackFromRole (role) < 0 && slotId < 0)
            return fail ("role must be \"fx\" (the master insert), \"inst\" (INST 1), \"inst2\", \"inst3\", \"insert_audio\", \"insert_inst1\" .. \"insert_inst3\", \"send1\" or \"send2\"");
        const bool asInst = role != "fx" && slotId < 0;
        const int loadTrack = slotId >= 0 ? slotId : asInst ? trackFromRole (role) : trkInst1;
        if (loadTrack >= trkInst2 && loadTrack < numTracks && req.hasProperty ("midi_channels"))
            midiFilters[loadTrack]->setMask (maskFromChannels (req["midi_channels"]));

        const auto path = str (req, "path");
        const auto name = str (req, "name");
        if (path.isNotEmpty())
        {
            auto* fmt = vst3Format();
            if (fmt == nullptr) return fail ("VST3 format not available");
            const juce::File f (path);
            if (! f.exists()) return fail ("no such file: " + path);
            juce::OwnedArray<juce::PluginDescription> found;
            knownPlugins.scanAndAddFile (f.getFullPathName(), true, found, *fmt);
            saveKnownPlugins();
            refreshRecentList();
            if (found.isEmpty()) return fail ("no plugin found in " + f.getFileName());
            loadPluginFromDescription (*found.getFirst(), asInst, loadTrack);
        }
        else if (name.isNotEmpty())
        {
            const juce::PluginDescription* hit = nullptr;
            auto types = knownPlugins.getTypes();
            for (const auto& t : types)
                if (t.isInstrument == asInst && t.name.equalsIgnoreCase (name)) { hit = &t; break; }
            if (hit == nullptr)
                for (const auto& t : types)
                    if (t.isInstrument == asInst && t.name.containsIgnoreCase (name)) { hit = &t; break; }
            if (hit == nullptr) return fail ("no cached " + role + " plugin matching \"" + name + "\" (use path=...vst3)");
            loadPluginFromDescription (*hit, asInst, loadTrack);
        }
        else
            return fail ("give \"path\" (a .vst3) or \"name\" (a cached plugin)");

        auto o = makeObj();
        put (o, "started", true);
        put (o, "note", "loading is asynchronous: poll status until busy is false");
        return o;
    }

    if (cmd == "remove_plugin")
    {
        const auto role = roleOf (req);
        if (role == "fx") removeEffect();
        else if (role == "inst" || role == "instrument" || role == "inst1") removeInstrument();
        else if (role == "inst2" || role == "inst3") removeExtraInstrument (trackFromRole (role));
        else if (slotFromRole (role) >= 0) removeSlotEffect (slotFromRole (role));
        else return fail ("role must be \"fx\", \"inst\", \"inst2\" or \"inst3\"");
        return makeObj();
    }

    if (cmd == "set_bypass")
    {
        bypassButton.setToggleState (flag (req, "on", true), juce::dontSendNotification);
        if (effectNode != nullptr && ! preRenderActive())
            effectNode->setBypassed (bypassButton.getToggleState());
        return makeObj();
    }

    //-- parameters ------------------------------------------------------------
    if (cmd == "list_params")
    {
        auto* proc = processorForRole (roleOf (req));
        if (proc == nullptr) return fail ("no plugin loaded for role \"" + roleOf (req) + "\"");
        const auto filter = str (req, "filter");
        const int limit = (int) num (req, "limit", 300);
        juce::Array<juce::var> list;
        const auto& params = proc->getParameters();
        for (int i = 0; i < params.size() && list.size() < limit; ++i)
        {
            auto* p = params[i];
            if (filter.isNotEmpty() && ! p->getName (128).containsIgnoreCase (filter))
                continue;
            auto e = makeObj();
            put (e, "index", i);
            put (e, "name", p->getName (128));
            put (e, "value", (double) p->getValue());
            put (e, "text", p->getCurrentValueAsText());
            put (e, "label", p->getLabel());
            put (e, "steps", p->getNumSteps());
            put (e, "discrete", p->isDiscrete());
            list.add (e);
        }
        auto o = makeObj();
        put (o, "count", params.size());
        put (o, "params", list);
        return o;
    }

    if (cmd == "set_param" || cmd == "set_params")
    {
        auto* proc = processorForRole (roleOf (req));
        if (proc == nullptr) return fail ("no plugin loaded for role \"" + roleOf (req) + "\"");
        const auto& params = proc->getParameters();

        auto findParam = [&params] (const juce::var& key) -> juce::AudioProcessorParameter*
        {
            if (key.isInt() || key.isInt64() || key.isDouble())
            {
                const int idx = (int) key;
                return idx >= 0 && idx < params.size() ? params[idx] : nullptr;
            }
            const auto n = key.toString();
            for (auto* p : params) if (p->getName (128).equalsIgnoreCase (n)) return p;
            for (auto* p : params) if (p->getName (128).containsIgnoreCase (n)) return p;
            return nullptr;
        };

        // set_param: {index|name, value|text}   set_params: {values:[{name|index, value|text}, ...]}
        juce::Array<juce::var> items;
        if (cmd == "set_param")
        {
            items.add (req);
        }
        else if (auto* a = req["values"].getArray())
            items = *a;
        else
            return fail ("set_params needs \"values\": [{name|index, value|text}, ...]");

        juce::Array<juce::var> results;
        for (const auto& it : items)
        {
            const juce::var key = it.hasProperty ("index") ? it["index"] : it["name"];
            auto* p = findParam (key);
            auto r = makeObj();
            if (p == nullptr)
            {
                put (r, "error", "no such parameter: " + key.toString());
                results.add (r);
                continue;
            }
            float v = p->getValue();
            if (it.hasProperty ("text"))
                v = p->getValueForText (it["text"].toString());
            else if (it.hasProperty ("value"))
                v = juce::jlimit (0.0f, 1.0f, (float) (double) it["value"]);
            else
            {
                put (r, "error", "give \"value\" (0..1) or \"text\"");
                results.add (r);
                continue;
            }
            p->beginChangeGesture();
            p->setValueNotifyingHost (v);
            p->endChangeGesture();
            put (r, "name", p->getName (128));
            put (r, "value", (double) p->getValue());
            put (r, "text", p->getCurrentValueAsText());
            results.add (r);
        }
        auto o = makeObj();
        put (o, "results", results);
        return o;
    }

    if (cmd == "save_state" || cmd == "load_state")
    {
        auto* proc = processorForRole (roleOf (req));
        if (proc == nullptr) return fail ("no plugin loaded for role \"" + roleOf (req) + "\"");
        const auto file = resolvePath (str (req, "path"), appDir());
        if (cmd == "save_state")
        {
            juce::MemoryBlock mb;
            proc->getStateInformation (mb);
            if (! file.replaceWithData (mb.getData(), mb.getSize())) return fail ("cannot write " + file.getFullPathName());
        }
        else
        {
            juce::MemoryBlock mb;
            if (! file.loadFileAsData (mb)) return fail ("cannot read " + file.getFullPathName());
            proc->setStateInformation (mb.getData(), (int) mb.getSize());
        }
        auto o = makeObj();
        put (o, "path", file.getFullPathName());
        return o;
    }

    if (cmd == "show_editor")
    {
        const auto role = roleOf (req);
        const bool want = flag (req, "show", true);
        if (role == "fx")
        {
            if ((editorWindow != nullptr) != want) toggleEditorFor (effectNode, currentEffectName, editorWindow);
        }
        else if (role == "inst2" || role == "inst3")
        {
            auto& e = extra (trackFromRole (role));
            if (e.node == nullptr) return fail ("no plugin loaded for role \"" + role + "\"");
            if ((e.editor != nullptr) != want) toggleEditorFor (e.node, e.name, e.editor);
        }
        else if (slotFromRole (role) >= 0)
        {
            auto& sl = fxSlot (slotFromRole (role));
            if (sl.node == nullptr) return fail ("no plugin loaded for role \"" + role + "\"");
            if ((sl.editor != nullptr) != want) toggleEditorFor (sl.node, sl.name, sl.editor);
        }
        else
        {
            if ((instEditorWindow != nullptr) != want) toggleEditorFor (instrumentNode, currentInstrumentName, instEditorWindow);
        }
        return makeObj();
    }

    if (cmd == "screenshot")
    {
        // target: main | fx | inst. The editor windows must be open for fx / inst.
        const auto target = str (req, "target", "main").toLowerCase();
        juce::Component* c = this;
        if (target == "fx")   c = editorWindow.get();
        if (target == "inst") c = instEditorWindow.get();
        if (c == nullptr) return fail ("that editor window is not open (show_editor first)");

        const auto img = c->createComponentSnapshot (c->getLocalBounds(), true, (float) num (req, "scale", 1.0));
        const auto file = resolvePath (str (req, "path", "shot_" + target + ".png"), appDir());
        juce::PNGImageFormat png;
        std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
        if (os == nullptr || ! png.writeImageToStream (img, *os)) return fail ("cannot write " + file.getFullPathName());
        os.reset();
        auto o = makeObj();
        put (o, "path", file.getFullPathName());
        put (o, "width", img.getWidth());
        put (o, "height", img.getHeight());
        return o;
    }

    //-- source / transport ----------------------------------------------------
    if (cmd == "set_source")
    {
        const auto m = str (req, "mode").toLowerCase();
        const int id = m == "live" ? srcLive : (m == "inst" || m == "instrument") ? srcInstrument : m == "file" ? srcFile : 0;
        if (id == 0) return fail ("mode must be live | inst | file");
        sourceCombo.setSelectedId (id, juce::sendNotificationSync);
        return makeObj();
    }

    if (cmd == "load_audio")
    {
        const juce::File f (str (req, "path"));
        if (! f.existsAsFile()) return fail ("no such file: " + str (req, "path"));
        loadAudioFile (f);
        return makeObj();
    }

    if (cmd == "play")
    {
        if (! filePlayer->hasFile()) return fail ("no audio loaded (load_audio / load_midi first)");
        auto& t = filePlayer->transport;
        if (t.hasStreamFinished() || t.getCurrentPosition() >= t.getLengthInSeconds() - 0.05)
            t.setPosition (0.0);
        if (req.hasProperty ("from"))
            t.setPosition ((double) req["from"]);
        t.start();
        return makeObj();
    }
    if (cmd == "stop")
    {
        filePlayer->transport.stop();
        if (flag (req, "rewind", true)) filePlayer->transport.setPosition (0.0);
        return makeObj();
    }
    if (cmd == "seek")
    {
        filePlayer->transport.setPosition (num (req, "seconds", 0.0));
        return makeObj();
    }
    if (cmd == "loop")
    {
        loopButton.setToggleState (flag (req, "on", true), juce::dontSendNotification);
        filePlayer->setLooping (loopButton.getToggleState());
        if (cacheSource != nullptr) cacheSource->setLooping (loopButton.getToggleState());
        return makeObj();
    }
    if (cmd == "prerender")
    {
        setPreRenderEnabled (flag (req, "on", true));
        auto o = makeObj();
        put (o, "prerender", preRenderActive());
        return o;
    }

    //-- MIDI file -> VSTi bounce (offline) -----------------------------------
    if (cmd == "load_midi")
    {
        const juce::File f (str (req, "path"));
        if (! f.existsAsFile()) return fail ("no such file: " + str (req, "path"));
        if (instrumentNode == nullptr) return fail ("load an instrument first (load_plugin role=inst)");
        // The file is about to be (re)loaded anyway: do not start a second bounce for the old file.
        if (req.hasProperty ("mpe") && flag (req, "mpe", false) != mpeConfig.enabled)
            setMpeEnabled (flag (req, "mpe", false), false);
        loadMidiFile (f);
        auto o = makeObj();
        put (o, "started", true);
        put (o, "analysis", mpeNote);
        put (o, "note", "bounce is asynchronous: poll status until busy is false, then play");
        return o;
    }

    // Write what the instrument actually receives (after TOP-BEND / zone setup) as an SMF.
    if (cmd == "export_midi")
    {
        if (midiSequence.getNumEvents() == 0) return fail ("no MIDI file loaded");
        const auto file = resolvePath (str (req, "path", "export.mid"), appDir());
        juce::MidiMessageSequence ticks;
        ticks.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0.0);
        for (int i = 0; i < midiSequence.getNumEvents(); ++i)
        {
            auto m = midiSequence.getEventPointer (i)->message;
            if (m.isTempoMetaEvent() || m.isEndOfTrackMetaEvent()) continue;
            m.setTimeStamp (m.getTimeStamp() * 1920.0);   // 120 bpm, 960 tpq
            ticks.addEvent (m);
        }
        ticks.addEvent (juce::MidiMessage::endOfTrack(), ticks.getEndTime() + 960.0);
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (960);
        mf.addTrack (ticks);
        file.deleteFile();
        juce::FileOutputStream out (file);
        if (! out.openedOk() || ! mf.writeTo (out)) return fail ("cannot write " + file.getFullPathName());
        auto o = makeObj();
        put (o, "path", file.getFullPathName());
        return o;
    }

    //-- fixed test rig: MIDI -> VSTi -> insert -> master, offline --------------
    if (cmd == "render_mix")   return startRenderMix (req);
    if (cmd == "rig_render")
        return startRigRender (req);

    //-- ARA ---------------------------------------------------------------------
    if (cmd == "ara_probe")
        return startAraProbe (req);

    //-- MPE -------------------------------------------------------------------
    if (cmd == "mpe")
    {
        if (req.hasProperty ("members"))   mpeConfig.members  = juce::jlimit (1, 15, (int) req["members"]);
        if (req.hasProperty ("member_pb")) mpeConfig.memberPB = juce::jlimit (1, 96, (int) req["member_pb"]);
        if (req.hasProperty ("master_pb")) mpeConfig.masterPB = juce::jlimit (1, 96, (int) req["master_pb"]);
        if (req.hasProperty ("on"))
            setMpeEnabled (flag (req, "on", true));
        else if (flag (req, "send", false))
            sendMpeSetupLive();
        return controlStatus()["mpe"];
    }

    //-- live MIDI -------------------------------------------------------------
    if (cmd == "midi_send")
    {
        // Immediate: ignores "t". For timing use midi_play.
        juce::String error;
        auto seq = sequenceFromEvents (req["events"], error);
        if (error.isNotEmpty()) return fail (error);
        for (int i = 0; i < seq.getNumEvents(); ++i)
            injectMidi (seq.getEventPointer (i)->message);
        auto o = makeObj();
        put (o, "sent", seq.getNumEvents());
        return o;
    }

    if (cmd == "midi_play" || cmd == "midi_play_file")
    {
        juce::MidiMessageSequence seq;
        if (cmd == "midi_play")
        {
            juce::String error;
            seq = sequenceFromEvents (req["events"], error);
            if (error.isNotEmpty()) return fail (error);
        }
        else
        {
            const juce::File f (str (req, "path"));
            juce::FileInputStream in (f);
            juce::MidiFile mf;
            if (! in.openedOk() || ! mf.readFrom (in)) return fail ("cannot read MIDI file " + f.getFullPathName());
            mf.convertTimestampTicksToSeconds();
            for (int t = 0; t < mf.getNumTracks(); ++t)
                seq.addSequence (*mf.getTrack (t), 0.0);
            seq.sort();
            juce::String note;
            seq = prepareMidiForInstrument (std::move (seq), note);
        }
        if (seq.getNumEvents() == 0) return fail ("no events");
        if (instrumentNode == nullptr && ! flag (req, "allow_no_inst", false))
            return fail ("no instrument loaded (the events would only reach FX / MIDI thru)");
        if (currentSourceMode() != srcInstrument)
            sourceCombo.setSelectedId (srcInstrument, juce::sendNotificationSync);
        midiScheduler.play (std::move (seq));
        auto o = makeObj();
        put (o, "duration", midiScheduler.getDuration());
        return o;
    }

    if (cmd == "midi_stop")
    {
        midiScheduler.stop();
        for (int ch = 1; ch <= 16; ++ch)
        {
            injectMidi (juce::MidiMessage::allNotesOff (ch));
            injectMidi (juce::MidiMessage::allSoundOff (ch));
        }
        return makeObj();
    }

    //-- hearing: record what the bench plays, measure wav files ---------------
    if (cmd == "record_start")
    {
        if (outputTap == nullptr) return fail ("no output tap");
        auto* dev = deviceManager.getCurrentAudioDevice();
        if (dev == nullptr) return fail ("no audio device is open");
        const auto file = resolvePath (str (req, "path", "capture.wav"), appDir());
        juce::String error;
        if (! outputTap->startRecording (file, dev->getCurrentSampleRate(), error)) return fail (error);
        const double seconds = num (req, "seconds", 0.0);
        if (seconds > 0.0)
            juce::Timer::callAfterDelay ((int) (seconds * 1000.0),
                [safe = juce::Component::SafePointer<MainComponent> (this)]
                { if (safe != nullptr && safe->outputTap != nullptr) safe->outputTap->stopRecording(); });
        auto o = makeObj();
        put (o, "path", file.getFullPathName());
        return o;
    }

    if (cmd == "record_stop")
    {
        if (outputTap == nullptr) return fail ("no output tap");
        auto* dev = deviceManager.getCurrentAudioDevice();
        const double rate = dev != nullptr ? dev->getCurrentSampleRate() : 48000.0;
        const auto n = outputTap->stopRecording();
        auto o = makeObj();
        put (o, "seconds", (double) n / rate);
        return o;
    }

    if (cmd == "analyze")
    {
        const auto file = resolvePath (str (req, "path"), appDir());
        std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (file));
        if (reader == nullptr) return fail ("cannot read " + file.getFullPathName());

        const double rate = reader->sampleRate;
        const int win = juce::jmax (1, (int) (rate * 0.25));
        juce::AudioBuffer<float> buf ((int) reader->numChannels, win);
        double peak = 0.0, sumSq = 0.0, sumDc = 0.0, firstSound = -1.0;
        juce::int64 total = 0, quiet = 0;
        juce::Array<juce::var> env;

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += win)
        {
            const int n = (int) juce::jmin ((juce::int64) win, reader->lengthInSamples - pos);
            reader->read (&buf, 0, n, pos, true, true);
            double wsq = 0.0;
            for (int c = 0; c < buf.getNumChannels(); ++c)
            {
                const float* d = buf.getReadPointer (c);
                for (int i = 0; i < n; ++i)
                {
                    peak = juce::jmax (peak, (double) std::abs (d[i]));
                    wsq += (double) d[i] * d[i];
                    sumDc += d[i];
                }
            }
            const double wrms = std::sqrt (wsq / (double) (n * juce::jmax (1, buf.getNumChannels())));
            sumSq += wsq;
            total += (juce::int64) n * juce::jmax (1, buf.getNumChannels());
            if (toDb (wrms) < -70.0) ++quiet;
            else if (firstSound < 0.0) firstSound = (double) pos / rate;
            if (env.size() < 240) env.add (juce::roundToInt (toDb (wrms) * 10.0) / 10.0);
        }

        auto o = makeObj();
        put (o, "path", file.getFullPathName());
        put (o, "seconds", (double) reader->lengthInSamples / rate);
        put (o, "sample_rate", rate);
        put (o, "channels", (int) reader->numChannels);
        put (o, "peak_db", toDb (peak));
        put (o, "rms_db", toDb (total > 0 ? std::sqrt (sumSq / (double) total) : 0.0));
        put (o, "dc_offset", total > 0 ? sumDc / (double) total : 0.0);
        put (o, "first_sound_s", firstSound);
        put (o, "silent_window_ratio", reader->lengthInSamples > 0 ? (double) quiet / std::ceil ((double) reader->lengthInSamples / win) : 1.0);
        put (o, "clipped", peak >= 0.999);
        put (o, "rms_db_per_quarter_second", env);
        return o;
    }

    //-- audio device ----------------------------------------------------------
    if (cmd == "audio_devices")
    {
        juce::Array<juce::var> types;
        for (auto* t : deviceManager.getAvailableDeviceTypes())
        {
            auto e = makeObj();
            put (e, "type", t->getTypeName());
            juce::Array<juce::var> outs;
            t->scanForDevices();
            for (const auto& n : t->getDeviceNames (false)) outs.add (n);
            put (e, "outputs", outs);
            types.add (e);
        }
        auto o = makeObj();
        put (o, "types", types);
        put (o, "audio", controlStatus()["audio"]);
        return o;
    }

    if (cmd == "set_audio")
    {
        if (req.hasProperty ("type"))
            deviceManager.setCurrentAudioDeviceType (str (req, "type"), true);
        auto setup = deviceManager.getAudioDeviceSetup();
        if (req.hasProperty ("output"))  setup.outputDeviceName = str (req, "output");
        if (req.hasProperty ("rate"))    setup.sampleRate = num (req, "rate", setup.sampleRate);
        if (req.hasProperty ("buffer"))  setup.bufferSize = (int) num (req, "buffer", setup.bufferSize);
        const auto err = deviceManager.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty()) return fail (err);
        rebuildConnections();
        return controlStatus()["audio"];
    }

    if (cmd == "help")
    {
        auto o = makeObj();
        put (o, "commands", juce::StringArray ({
            "ping", "status", "graph_dump", "track_set", "track_status", "render_mix", "list_plugins", "load_plugin", "remove_plugin", "set_bypass",
            "list_params", "set_param", "set_params", "save_state", "load_state",
            "show_editor", "screenshot", "set_source", "load_audio", "play", "stop", "seek", "loop",
            "prerender", "rig_render", "ara_probe", "load_midi", "export_midi", "mpe", "midi_send", "midi_play", "midi_play_file", "midi_stop",
            "record_start", "record_stop", "analyze", "audio_devices", "set_audio" }).joinIntoString (" "));
        put (o, "doc", "docs/CONTROL.md");
        return o;
    }

    return fail ("unknown cmd \"" + cmd + "\" (try cmd=help)");
}

//==============================================================================
// rig_render. One track at the top level:
//     inst=<name|path> | source=impulse [impulse_at] [impulse_amp]
//     | source=file source_path=<wav> [clip_start=<s on the timeline>] [clip_offset=<s into the file>] [clip_length=<s>] [clip_gain_db]
//     [insert=<name|path> | delay_actual=<n> [delay_declared=<m>] | tail_t60=<s> [tail_declared=<s>]]
//     events=[...]  [inst_state|insert_state=<file>]  [inst_params|insert_params={name: 0..1}]
//   or several: tracks=[ {the same keys}, ... ]  (summed; the graph aligns them by their declared latencies)
// then   [master=<name|path> | master_tail_t60 ...] [master_state] [master_params]
//        out=<wav> [rate] [block] [tail] [compensate] [dry_parallel] [settle=<ms, default 500>] [timeout=<s, 600>]
// The bench only resolves names to plugin descriptions and writes a job file; a separate worker process
// (this exe with --rig-worker, see RigWorkerClient.h / Main.cpp) loads, renders and tears down. A crash or
// hang there is reported in status.rig and never reaches the bench.
// Asynchronous like the MIDI bounce: poll status until busy is false, then read status.rig.
//==============================================================================
juce::var MainComponent::startRigRender (const juce::var& req)
{
    if (rigWorker.isRunning())
        return fail ("a rig render is already running");

    auto* fmt = vst3Format();
    if (fmt == nullptr) return fail ("VST3 format not available");

    // resolves a plugin name or .vst3 path to its description
    auto findPlugin = [&] (const juce::String& what, bool wantInst, juce::PluginDescription& desc) -> bool
    {
        if (what.endsWithIgnoreCase (".vst3") || juce::File::isAbsolutePath (what))
        {
            juce::OwnedArray<juce::PluginDescription> types;
            knownPlugins.scanAndAddFile (juce::File (what).getFullPathName(), true, types, *fmt);
            if (types.isEmpty()) return false;
            desc = *types.getFirst();
            return true;
        }
        auto types = knownPlugins.getTypes();
        for (const auto& t : types)
            if (t.isInstrument == wantInst && t.name.equalsIgnoreCase (what)) { desc = t; return true; }
        for (const auto& t : types)
            if (t.isInstrument == wantInst && t.name.containsIgnoreCase (what)) { desc = t; return true; }
        return false;
    };

    // One slot (`role`) of a request object `src`: returns a stage object, or a void var if the slot is empty.
    // Test doubles (delay / tail) take the place of a plugin in the insert and master slots.
    auto stageFor = [&] (const juce::var& src, const juce::String& role, bool wantInst, juce::String& error) -> juce::var
    {
        auto stage = makeObj();
        put (stage, "role", role);
        // test doubles (delay / tail / probe) stand in for the FX of the insert, the master and the send buses (keys: bus1_delay_actual ...)
        const bool isDoubleSlot = role == "insert" || role == "master" || role.startsWith ("bus");
        const juce::String slotPrefix = role == "master" ? juce::String ("master_") : role.startsWith ("bus") ? role + "_" : juce::String();

        if (isDoubleSlot && src.hasProperty (slotPrefix + "delay_actual"))
        {
            const auto prefix = slotPrefix;
            put (stage, "kind", "delay");
            put (stage, "delay_actual", (int) num (src, (prefix + "delay_actual").toRawUTF8(), 0.0));
            put (stage, "delay_declared", (int) num (src, (prefix + "delay_declared").toRawUTF8(),
                                                     num (src, (prefix + "delay_actual").toRawUTF8(), 0.0)));
            return stage;
        }
        // test doubles that report what the host does: "playhead" (transport info written into the audio) and "gain"
        // (a host parameter that scales the audio)
        if (isDoubleSlot && str (src, (slotPrefix + "probe").toRawUTF8()).isNotEmpty())
        {
            put (stage, "kind", "probe");
            put (stage, "probe", str (src, (slotPrefix + "probe").toRawUTF8()));
            return stage;
        }
        if (isDoubleSlot && src.hasProperty (slotPrefix + "tail_t60"))
        {
            const auto prefix = slotPrefix;
            put (stage, "kind", "tail");
            put (stage, "tail_t60", num (src, (prefix + "tail_t60").toRawUTF8(), 1.0));
            put (stage, "tail_declared", num (src, (prefix + "tail_declared").toRawUTF8(),
                                              num (src, (prefix + "tail_t60").toRawUTF8(), 1.0)));
            return stage;
        }

        const auto what = str (src, role.toRawUTF8());
        if (what.isEmpty())
            return {};
        juce::PluginDescription desc;
        // an ARA plug-in (Melodyne, SpectraLayers) may be listed as an instrument or as an effect
        if (! findPlugin (what, wantInst, desc) && ! (role == "ara" && findPlugin (what, ! wantInst, desc)))
        {
            error = role + ": no plugin matching \"" + what + "\"";
            return {};
        }
        put (stage, "kind", "plugin");
        put (stage, "desc_xml", desc.createXml()->toString());
        const auto stateKey = role + "_state";
        if (str (src, stateKey.toRawUTF8()).isNotEmpty())
        {
            const auto f = resolvePath (str (src, stateKey.toRawUTF8()), appDir());
            if (! f.existsAsFile()) { error = "cannot read " + stateKey; return {}; }
            put (stage, "state", f.getFullPathName());
        }
        const auto paramsKey = role + "_params";
        if (src.hasProperty (paramsKey.toRawUTF8()))
            put (stage, "params", src[paramsKey.toRawUTF8()]);
        return stage;
    };

    // A channel strip stage (the live bench's TrackStrip) when the request object sets gain_db / balance / mute
    // (with `prefix` "master_" for the master's), else void: no stage, as before.
    auto stripFor = [&] (const juce::var& src, const juce::String& prefix) -> juce::var
    {
        const auto g = prefix + "gain_db", b = prefix + "balance", m = prefix + "mute";
        if (! (src.hasProperty (g.toRawUTF8()) || src.hasProperty (b.toRawUTF8()) || src.hasProperty (m.toRawUTF8())))
            return {};
        auto stage = makeObj();
        put (stage, "role", "strip");
        put (stage, "kind", "strip");
        put (stage, "gain_db", num (src, g.toRawUTF8(), 0.0));
        put (stage, "balance", num (src, b.toRawUTF8(), 0.0));
        put (stage, "mute", flag (src, m.toRawUTF8(), false));
        return stage;
    };

    // one track from a request object (the top level, or one entry of "tracks")
    auto trackFor = [&] (const juce::var& t, juce::String& error) -> juce::var
    {
        auto track = makeObj();
        const auto source = str (t, "source", "midi");
        put (track, "source", source);
        juce::Array<juce::var> stages;

        if (source == "impulse")
        {
            put (track, "impulse_at", num (t, "impulse_at", 1000.0));
            put (track, "impulse_amp", num (t, "impulse_amp", 0.1));
        }
        else if (source == "file" || source == "ara")
        {
            const auto f = resolvePath (str (t, "source_path"), appDir());
            if (! f.existsAsFile()) { error = "source_path: no such file " + f.getFullPathName(); return {}; }
            put (track, "source_path", f.getFullPathName());
            for (const char* key : { "clip_start", "clip_offset", "clip_length", "clip_gain_db" })
                if (t.hasProperty (key))
                    put (track, key, t[key]);

            if (source == "ara")
            {
                // the ARA plug-in is the track's first stage and its source: it reads the file through an ARA document
                auto ara = stageFor (t, "ara", false, error);
                if (! error.isEmpty()) return {};
                if (ara.isVoid()) { error = "give \"ara\" (a plug-in name or a .vst3 path)"; return {}; }
                stages.add (ara);
            }
        }
        else
        {
            sequenceFromEvents (t["events"], error);   // validate here; the worker converts again
            if (error.isNotEmpty()) return {};
            put (track, "events", t["events"]);
            auto inst = stageFor (t, "inst", true, error);
            if (error.isNotEmpty()) return {};
            if (inst.isVoid()) { error = "give \"inst\" (a cached name or a .vst3 path)"; return {}; }
            stages.add (inst);
        }

        auto insert = stageFor (t, "insert", false, error);
        if (error.isNotEmpty()) return {};
        if (! insert.isVoid())
            stages.add (insert);
        if (auto strip = stripFor (t, {}); ! strip.isVoid())
            stages.add (strip);
        put (track, "stages", stages);
        for (const char* key : { "send1_db", "send2_db" })        // post-fader sends to the two buses
            if (t.hasProperty (key))
                put (track, key, t[key]);
        return track;
    };

    auto job = makeObj();
    put (job, "rate",         num (req, "rate", 48000.0));
    put (job, "block",        juce::jlimit (32, 8192, (int) num (req, "block", 512)));
    put (job, "tail",         juce::jmax (0.0, num (req, "tail", 2.0)));
    put (job, "compensate",   flag (req, "compensate", true));
    put (job, "dry_parallel", flag (req, "dry_parallel", false));
    put (job, "settle",       juce::jlimit (0, 10000, (int) num (req, "settle", 500.0)));
    put (job, "out",          resolvePath (str (req, "out", "rig.wav"), appDir()).getFullPathName());

    put (job, "bpm", num (req, "bpm", 120.0));
    put (job, "automation_quantised", flag (req, "automation_quantised", false));
    if (req.hasProperty ("time_sig"))       put (job, "time_sig", req["time_sig"]);
    if (req.hasProperty ("block_pattern"))  put (job, "block_pattern", req["block_pattern"]);
    if (req.hasProperty ("automation"))     put (job, "automation", req["automation"]);

    juce::String error;
    juce::Array<juce::var> tracks;
    if (auto* list = req["tracks"].getArray())
    {
        for (const auto& t : *list)
        {
            auto track = trackFor (t, error);
            if (error.isNotEmpty()) return fail ("tracks[" + juce::String (tracks.size()) + "]: " + error);
            tracks.add (track);
        }
        if (tracks.isEmpty()) return fail ("\"tracks\" is empty");
    }
    else
    {
        auto track = trackFor (req, error);
        if (error.isNotEmpty()) return fail (error);
        tracks.add (track);
    }
    put (job, "tracks", tracks);

    juce::Array<juce::var> master;
    auto m = stageFor (req, "master", false, error);
    if (error.isNotEmpty()) return fail (error);
    if (! m.isVoid())
        master.add (m);
    if (auto strip = stripFor (req, "master_"); ! strip.isVoid())
        master.add (strip);
    put (job, "master", master);

    for (int n = 1; n <= 2; ++n)                                    // send buses: bus1 / bus2 = the FX, bus1_gain_db ... = the return strip
    {
        const auto role = "bus" + juce::String (n);
        auto fx = stageFor (req, role, false, error);
        if (! error.isEmpty()) return fail (error);
        if (fx.isVoid())
            continue;
        juce::Array<juce::var> stages;
        stages.add (fx);
        if (auto ret = stripFor (req, role + "_"); ! ret.isVoid())
            stages.add (ret);
        put (job, role, stages);
    }

    rigResult = juce::var();
    rigWorker.onDone = [safe = juce::Component::SafePointer<MainComponent> (this)] (juce::var r)
    {
        if (safe == nullptr)
            return;
        safe->rigResult = r;
        auto done = std::move (safe->rigExtraDone);     // a caller that wants to act on the result (the MIDI bounce)
        safe->rigExtraDone = nullptr;
        if (done)
            done (r);
    };
    if (! rigWorker.start (job, num (req, "timeout", 600.0), appDir(), error)) return fail (error);

    auto o = makeObj();
    put (o, "started", true);
    put (o, "note", "asynchronous, in a worker process: poll status until busy is false, then read status.rig");
    return o;
}

//==============================================================================
// render_mix: the mixer as it is now, rendered offline by the rig (docs/TRACKS.md, P4). Which tracks sound, their gain and
// balance, the MASTER FX and its strip are read from the live bench; the AUDIO track contributes its file, an INST
// track contributes the loaded MIDI file's events on the channels it listens to. Not rendered: the live input (it is not a
// file) and an instrument track when no MIDI file is loaded. out=<wav>, tail (s), compensate, rate, block.
//==============================================================================
namespace
{
    // a MIDI sequence (seconds) as the events the rig takes; channels outside `mask` are left out
    juce::var eventsFromSequence (const juce::MidiMessageSequence& seq, juce::uint32 mask)
    {
        juce::Array<juce::var> list;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            const int ch = m.getChannel();
            if (ch == 0 || (mask & (1u << (ch - 1))) == 0)
                continue;
            auto e = makeObj();
            put (e, "t", m.getTimeStamp());
            put (e, "ch", ch);
            if (m.isNoteOn (false) && m.getVelocity() > 0)  { put (e, "type", "note_on");  put (e, "note", m.getNoteNumber()); put (e, "vel", (int) m.getVelocity()); }
            else if (m.isNoteOff (true))                    { put (e, "type", "note_off"); put (e, "note", m.getNoteNumber()); }
            else if (m.isPitchWheel())                      { put (e, "type", "pitch_bend"); put (e, "value", m.getPitchWheelValue()); }
            else if (m.isChannelPressure())                 { put (e, "type", "pressure"); put (e, "value", m.getChannelPressureValue()); }
            else if (m.isAftertouch())                      { put (e, "type", "poly_pressure"); put (e, "note", m.getNoteNumber()); put (e, "value", m.getAfterTouchValue()); }
            else if (m.isController())                      { put (e, "type", "cc"); put (e, "cc", m.getControllerNumber()); put (e, "value", m.getControllerValue()); }
            else if (m.isProgramChange())                   { put (e, "type", "program"); put (e, "value", m.getProgramChangeNumber()); }
            else continue;
            list.add (e);
        }
        return list;
    }
}

juce::var MainComponent::buildMixRequest (const MixOptions& opt, const juce::var& req, juce::StringArray& left)
{
    auto rq = makeObj();
    juce::Array<juce::var> tracks;
    const auto dir = appDir();

    auto saveState = [&dir] (juce::AudioProcessor& p, const juce::String& name)
    {
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        const auto f = dir.getChildFile ("rendermix_" + name + ".state");
        f.replaceWithData (mb.getData(), mb.getSize());
        return f.getFullPathName();
    };

    // a track's insert and its post-fader sends (the sends only when the buses are part of the render)
    auto addInsertAndSends = [&] (juce::var& t, int tr)
    {
        if (insertSlot[tr].node != nullptr)
        {
            put (t, "insert", insertSlot[tr].name);
            put (t, "insert_state", saveState (*insertSlot[tr].node->getProcessor(), "insert" + juce::String (tr)));
        }
        if (opt.master)
            for (int n = 0; n < 2; ++n)
                if (busSlot[n].node != nullptr)
                    put (t, ("send" + juce::String (n + 1) + "_db").toRawUTF8(), (double) sendLevel[tr][n]->getGainDb());
    };

    // --- AUDIO: the file in the player (not when it is itself the bounce of the MIDI file: the instruments are rendered directly) ---
    const auto takes = [&] (int tr) { return opt.sounding ? strips[tr]->isSounding() : strips[tr]->isAudible(); };
    if (opt.audio && takes (trkAudio))
    {
        if (midiChainActive() && ! opt.audioWhenChain) left.add ("AUDIO (it is the bounce of the MIDI file: the instruments are rendered directly)");
        else if (currentSourceMode() != srcFile)    left.add ("AUDIO (the live input is not a file)");
        else if (! filePlayer->hasFile())           left.add ("AUDIO (no file loaded)");
        else
        {
            auto t = makeObj();
            put (t, "source", "file");
            put (t, "source_path", effectivePlayableFile().getFullPathName());
            put (t, "gain_db", (double) strips[trkAudio]->getGainDb());
            put (t, "balance", (double) strips[trkAudio]->getBalance());
            addInsertAndSends (t, trkAudio);
            tracks.add (t);
        }
    }

    // --- INST 1..3: the loaded MIDI file through the instrument, on the channels the track listens to ---
    for (int tr = trkInst1; opt.insts && tr < numTracks; ++tr)
    {
        const auto inst = tr == trkInst1 ? instrumentNode : extra (tr).node;
        if (inst == nullptr || ! takes (tr))
            continue;
        const auto name = "INST " + juce::String (tr);
        if (midiSequence.getNumEvents() == 0)       { left.add (name + " (no MIDI file loaded)"); continue; }
        const auto events = eventsFromSequence (midiSequence, midiFilters[tr]->getMask());   // midiSequence is already prepared (MPE, TOP-BEND)
        if (events.getArray() == nullptr || events.getArray()->isEmpty())
            { left.add (name + " (no MIDI on its channels)"); continue; }

        auto t = makeObj();
        put (t, "source", "midi");
        put (t, "events", events);
        put (t, "inst", tr == trkInst1 ? currentInstrumentName : extra (tr).name);
        put (t, "inst_state", saveState (*inst->getProcessor(), "inst" + juce::String (tr)));
        put (t, "gain_db", (double) strips[tr]->getGainDb());
        put (t, "balance", (double) strips[tr]->getBalance());
        addInsertAndSends (t, tr);
        tracks.add (t);
    }
    put (rq, "tracks", tracks);

    // --- MASTER: the FX insert (unless bypassed) and the master strip ---
    if (opt.master)
    {
        if (effectNode != nullptr && ! bypassButton.getToggleState())
        {
            put (rq, "master", currentEffectName);
            put (rq, "master_state", saveState (*effectNode->getProcessor(), "master"));
        }
        put (rq, "master_gain_db", (double) masterStrip->getGainDb());
        put (rq, "master_balance", (double) masterStrip->getBalance());

        // the send buses: the FX, and the return strip
        for (int n = 0; n < 2; ++n)
            if (busSlot[n].node != nullptr)
            {
                const auto role = "bus" + juce::String (n + 1);
                put (rq, role.toRawUTF8(), busSlot[n].name);
                put (rq, (role + "_state").toRawUTF8(), saveState (*busSlot[n].node->getProcessor(), role));
                put (rq, (role + "_gain_db").toRawUTF8(), (double) busReturn[n]->getGainDb());
                put (rq, (role + "_balance").toRawUTF8(), (double) busReturn[n]->getBalance());
                put (rq, (role + "_mute").toRawUTF8(), busReturn[n]->isMuted());
            }
    }

    const auto setup = deviceManager.getAudioDeviceSetup();
    put (rq, "rate", req.hasProperty ("rate") ? req["rate"] : juce::var (setup.sampleRate > 0 ? setup.sampleRate : 48000.0));
    put (rq, "block", req.hasProperty ("block") ? req["block"] : juce::var (512));
    put (rq, "tail", req.hasProperty ("tail") ? req["tail"] : juce::var (2.0));
    put (rq, "compensate", flag (req, "compensate", true));
    put (rq, "out", str (req, "out", "mix.wav"));
    if (req.hasProperty ("timeout")) put (rq, "timeout", req["timeout"]);
    return rq;
}

juce::var MainComponent::startRenderMix (const juce::var& req)
{
    juce::StringArray left;
    auto rq = buildMixRequest ({}, req, left);
    if (rq["tracks"].getArray() == nullptr || rq["tracks"].getArray()->isEmpty())
        return fail ("nothing to render: no sounding track with a file (AUDIO) or a loaded MIDI file (INST)"
                     + (left.isEmpty() ? juce::String() : " - left out: " + left.joinIntoString ("; ")));

    auto r = startRigRender (rq);
    if (auto* o = r.getDynamicObject())
        if (! left.isEmpty())
            o->setProperty ("left_out", left.joinIntoString ("; "));
    return r;
}

// Is a bounce by the in-process engine (one instrument, every channel: fast, the same audio) not enough?
// INST 2 / 3 with MIDI on their channels, or INST 1 listening to only some channels, need the rig.
bool MainComponent::instrumentsNeedRig() const
{
    auto hasEvents = [this] (int t)
    {
        const auto mask = midiFilters[t]->getMask();
        for (int i = 0; i < midiSequence.getNumEvents(); ++i)
        {
            const int ch = midiSequence.getEventPointer (i)->message.getChannel();
            if (ch >= 1 && (mask & (1u << (ch - 1))) != 0)
                return true;
        }
        return false;
    };
    for (int t = trkInst2; t < numTracks; ++t)
        if (extra (t).node != nullptr && hasEvents (t))
            return true;
    return instrumentNode != nullptr && midiFilters[trkInst1]->getMask() != 0xFFFFu;
}

// PRE-RENDER by the rig: the whole mix as the live graph plays it now (the sounding tracks, inserts, sends and buses, the master FX
// and strip) rendered offline in the worker process; the result becomes the cache that plays.
void MainComponent::startMixBake()
{
    if (mixBakeRunning || rigWorker.isRunning())
    {
        markBakeDirty();                    // one job at a time: the timer tries again
        return;
    }
    mixBakeDirty = false;

    ++bakeGeneration;
    const auto out = appDir().getChildFile (juce::String::formatted ("mixbake_%04d.wav", bakeGeneration));
    auto o = makeObj();
    put (o, "out", out.getFullPathName());
    put (o, "tail", 2.0);
    put (o, "compensate", true);
    MixOptions opt;
    opt.sounding = true;
    opt.audioWhenChain = true;
    opt.insts = false;                      // PRE-RENDER is file mode: the instruments are inside the file (the MIDI chain's bounce)
    juce::StringArray left;
    auto rq = buildMixRequest (opt, o, left);
    if (rq["tracks"].getArray() == nullptr || rq["tracks"].getArray()->isEmpty())
    {
        setStatus ("PRE-RENDER: nothing sounding to bake (the AUDIO track is muted?)");
        setPreRenderEnabled (false);
        return;
    }
    const auto r = startRigRender (rq);
    if (r.hasProperty ("error"))
    {
        setStatus ("PRE-RENDER: the rig did not start: " + r["error"].toString());
        setPreRenderEnabled (false);
        return;
    }

    mixBakeRunning = true;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    rigExtraDone = [safe = juce::Component::SafePointer<MainComponent> (this), out, t0] (const juce::var& result)
    {
        if (safe == nullptr)
            return;
        safe->mixBakeRunning = false;
        const bool ok = (bool) result.getProperty ("ok", false);
        juce::String info = result["error"].toString();
        if (ok)
        {
            const double seconds = (double) result.getProperty ("samples", 0) / juce::jmax (1.0, (double) result.getProperty ("sample_rate", 48000.0));
            info = juce::String::formatted ("rig, %.1f s in %.1f s", seconds, (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
        }
        safe->finishMixBake (ok, out, info);
    };
}

void MainComponent::finishMixBake (bool ok, const juce::File& out, const juce::String& info)
{
    if (! preRenderActive() || ! mixBakeMode)       // switched off while the rig ran
    {
        out.deleteFile();
        return;
    }
    if (! ok)
    {
        out.deleteFile();
        renderLabel.setText ("PRE-RENDER: bake FAILED: " + info, juce::dontSendNotification);
        setStatus ("PRE-RENDER bake failed: " + info);
        return;
    }
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (out));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 150'000'000)
    {
        out.deleteFile();
        renderLabel.setText ("PRE-RENDER: could not read the bake", juce::dontSendNotification);
        return;
    }

    // fill the cache that is not playing, then swap (the playing one is never written)
    const int next = 1 - bakeSlotInUse;
    auto& c = bakeCache[next];
    const auto n = (int) reader->lengthInSamples;
    c.data.setSize (2, n, false, true, true);
    reader->read (&c.data, 0, n, 0, true, true);
    if (reader->numChannels == 1)
        c.data.copyFrom (1, 0, c.data, 0, 0, n);
    c.length = n;
    c.sampleRate = reader->sampleRate;
    c.valid.store (n);
    c.primed.store (true);
    reader.reset();
    if (bakeSource[next] == nullptr)
        bakeSource[next] = std::make_unique<CacheAudioSource> (c);
    bakeSource[next]->setLooping (loopButton.getToggleState());

    const bool playing = filePlayer->transport.isPlaying();
    filePlayer->attachExternalSource (bakeSource[next].get(), c.sampleRate);
    if (playing)
        filePlayer->transport.start();
    bakeSlotInUse = next;

    const bool firstBake = ! mixBakeReady;
    mixBakeReady = true;
    if (firstBake)
        rebuildConnections();                        // from now on the cache is the sound: strips, inserts, sends and FX leave the live path
    if (currentBakeFile.existsAsFile() && currentBakeFile != out)
        currentBakeFile.deleteFile();
    currentBakeFile = out;
    renderLabel.setText ("PRE-RENDER: mix baked (" + info + ")", juce::dontSendNotification);
}

bool MainComponent::startMidiMixBounce()
{
    ++bounceGeneration;
    const auto out = appDir().getChildFile (juce::String::formatted ("midi_bounce_%04d.wav", bounceGeneration));
    auto o = makeObj();
    put (o, "out", out.getFullPathName());
    put (o, "tail", 4.0);          // the ring-out the in-process bounce gives
    put (o, "compensate", true);

    juce::StringArray left;
    auto rq = buildMixRequest ({ false, true, false }, o, left);     // the instruments only: AUDIO and the master FX stay live
    if (rq["tracks"].getArray() == nullptr || rq["tracks"].getArray()->isEmpty())
    {
        firstBouncePending = false;
        midiStatusLabel.setText ("MIDI: no instrument track has MIDI on its channels", juce::dontSendNotification);
        setStatus ("No instrument track listens to a channel the MIDI file uses (set the channels in the strips).");
        return false;
    }

    const auto r = startRigRender (rq);
    if (r.hasProperty ("error"))                    // fail() answers with an "error"
    {
        firstBouncePending = false;
        midiStatusLabel.setText ("MIDI: rig bounce did not start: " + r["error"].toString(), juce::dontSendNotification);
        return false;
    }

    multiBounceRunning = true;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    rigExtraDone = [safe = juce::Component::SafePointer<MainComponent> (this), out, t0] (const juce::var& result)
    {
        if (safe == nullptr)
            return;
        safe->multiBounceRunning = false;
        const bool ok = (bool) result.getProperty ("ok", false);
        juce::String info = result["error"].toString();
        if (ok)
        {
            const double seconds = (double) result.getProperty ("samples", 0) / juce::jmax (1.0, (double) result.getProperty ("sample_rate", 48000.0));
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            info = juce::String::formatted ("rig, %.1fx realtime", elapsed > 0.01 ? seconds / elapsed : 0.0);
        }
        safe->handleBounceDone (ok, out, info);
    };
    midiStatusLabel.setText ("MIDI: bouncing the instrument tracks (rig) ...", juce::dontSendNotification);
    return true;
}

//==============================================================================
// ara_probe: name=<plug-in> (any cached plug-in, instrument or effect) or path=<.vst3>
// Asks the VST3 format for the plug-in's ARA factory WITHOUT creating an instance, and reports what the factory
// declares. Asynchronous: poll status until busy is false, then read status.ara_probe.
//==============================================================================
juce::var MainComponent::startAraProbe (const juce::var& req)
{
#if JUCE_PLUGINHOST_ARA
    if (araProbePending) return fail ("an ARA probe is already running");

    const auto what = str (req, "name", str (req, "path"));
    if (what.isEmpty()) return fail ("give \"name\" (a cached plug-in) or \"path\" (a .vst3)");

    juce::PluginDescription desc;
    bool found = false;
    if (what.endsWithIgnoreCase (".vst3") || juce::File::isAbsolutePath (what))
    {
        if (auto* fmt = vst3Format())
        {
            juce::OwnedArray<juce::PluginDescription> types;
            knownPlugins.scanAndAddFile (juce::File (what).getFullPathName(), true, types, *fmt);
            if (! types.isEmpty()) { desc = *types.getFirst(); found = true; }
        }
    }
    else
    {
        for (const auto& t : knownPlugins.getTypes())
            if (t.name.equalsIgnoreCase (what)) { desc = t; found = true; break; }
        if (! found)
            for (const auto& t : knownPlugins.getTypes())
                if (t.name.containsIgnoreCase (what)) { desc = t; found = true; break; }
    }
    if (! found) return fail ("no plug-in matching \"" + what + "\"");

    araProbePending = true;
    araProbeResult = juce::var();
    formatManager.createARAFactoryAsync (desc,
        [safe = juce::Component::SafePointer<MainComponent> (this), name = desc.name] (juce::ARAFactoryResult result)
        {
            if (safe == nullptr)
                return;
            auto r = makeObj();
            put (r, "plugin", name);
            if (auto* f = result.araFactory.get())
            {
                put (r, "ara", true);
                put (r, "plug_in_name", juce::String (f->plugInName));
                put (r, "manufacturer", juce::String (f->manufacturerName));
                put (r, "version", juce::String (f->version));
                put (r, "factory_id", juce::String (f->factoryID));
                put (r, "ara_api_generation_lowest", (int) f->lowestSupportedApiGeneration);
                put (r, "ara_api_generation_highest", (int) f->highestSupportedApiGeneration);
                put (r, "document_archive_id", juce::String (f->documentArchiveID));
                put (r, "supported_playback_transformations", (juce::int64) f->supportedPlaybackTransformationFlags);
                juce::Array<juce::var> types;
                for (ARA::ARASize i = 0; i < f->analyzeableContentTypesCount; ++i)
                    types.add ((int) f->analyzeableContentTypes[i]);
                put (r, "analyzeable_content_types", types);
            }
            else
            {
                put (r, "ara", false);
                put (r, "error", result.errorMessage);
            }
            safe->araProbeResult = r;
            safe->araProbePending = false;
        });

    auto o = makeObj();
    put (o, "started", true);
    put (o, "note", "asynchronous: poll status until busy is false, then read status.ara_probe");
    return o;
#else
    juce::ignoreUnused (req);
    return fail ("this build has no ARA hosting (the ARA SDK was not found when it was configured)");
#endif
}
