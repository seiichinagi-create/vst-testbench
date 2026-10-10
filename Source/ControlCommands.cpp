#include "MainComponent.h"

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
juce::AudioProcessor* MainComponent::processorForRole (const juce::String& role) const
{
    if (role == "fx")
        return effectNode != nullptr ? effectNode->getProcessor() : nullptr;
    if (role == "inst" || role == "instrument")
        return instrumentNode != nullptr ? instrumentNode->getProcessor() : nullptr;
    return nullptr;
}

//==============================================================================
// {"t":0.5,"type":"note_on","ch":2,"note":60,"vel":100,"dur":1.0}
// types: note_on note_off pitch_bend pressure poly_pressure cc program all_off
juce::MidiMessageSequence MainComponent::sequenceFromEvents (const juce::var& events, juce::String& error) const
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

//==============================================================================
juce::var MainComponent::controlStatus() const
{
    auto o = makeObj();

    const int mode = currentSourceMode();
    put (o, "source", mode == srcLive ? "live" : mode == srcInstrument ? "inst" : "file");
    put (o, "status_text", statusLabel.getText());

    const bool busy = pendingLoads > 0 || bounceEngine.isBouncing() || offlineInstLoading || offlineFxLoading
                      || firstBouncePending || (preRenderActive() && renderEngine.isRendering())
                      || (preRenderActive() && fxStale.load());
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
        put (m, "bouncing", bounceEngine.isBouncing());
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
        put (p, "top_bend", topBend);
        put (p, "members", mpeConfig.members);
        put (p, "member_pitchbend_semitones", mpeConfig.memberPB);
        put (p, "master_pitchbend_semitones", mpeConfig.masterPB);
        put (o, "mpe", p);
    }
    put (o, "prerender", preRenderActive());
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
        if (role != "fx" && role != "inst" && role != "instrument")
            return fail ("role must be \"fx\" or \"inst\"");
        const bool asInst = role != "fx";

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
            loadPluginFromDescription (*found.getFirst(), asInst);
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
            loadPluginFromDescription (*hit, asInst);
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
        else if (role == "inst" || role == "instrument") removeInstrument();
        else return fail ("role must be \"fx\" or \"inst\"");
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
        if (req.hasProperty ("top_bend") && flag (req, "top_bend", false) != topBend)
            setTopBend (flag (req, "top_bend", false), false);
        loadMidiFile (f);
        auto o = makeObj();
        put (o, "started", true);
        put (o, "analysis", mpeNote);
        put (o, "note", "bounce is asynchronous: poll status until busy is false, then play");
        return o;
    }

    if (cmd == "top_bend")
    {
        setTopBend (flag (req, "on", true));
        auto o = makeObj();
        put (o, "top_bend", topBend);
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
            "ping", "status", "list_plugins", "load_plugin", "remove_plugin", "set_bypass",
            "list_params", "set_param", "set_params", "save_state", "load_state",
            "show_editor", "screenshot", "set_source", "load_audio", "play", "stop", "seek", "loop",
            "prerender", "load_midi", "top_bend", "export_midi", "mpe", "midi_send", "midi_play", "midi_play_file", "midi_stop",
            "record_start", "record_stop", "analyze", "audio_devices", "set_audio" }).joinIntoString (" "));
        put (o, "doc", "docs/CONTROL.md");
        return o;
    }

    return fail ("unknown cmd \"" + cmd + "\" (try cmd=help)");
}
