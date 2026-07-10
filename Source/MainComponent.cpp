#include "MainComponent.h"

namespace
{
    // Small window that owns a plugin editor and cleans it up on close.
    class PluginEditorWindow : public juce::DocumentWindow
    {
    public:
        PluginEditorWindow (const juce::String& name, juce::AudioProcessorEditor* editor)
            : DocumentWindow (name, juce::Colours::black, DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            centreWithSize (juce::jmax (200, editor->getWidth()),
                            juce::jmax (100, editor->getHeight()));
            setVisible (true);
        }

        void closeButtonPressed() override { setVisible (false); }
    };
}

//==============================================================================
MainComponent::MainComponent()
{
    juce::addDefaultFormatsToManager (formatManager);
    audioFormats.registerBasicFormats();
    loadKnownPlugins();

    // --- audio + MIDI device ---
    std::unique_ptr<juce::XmlElement> savedAudio (juce::XmlDocument::parse (audioStateFile()));
    deviceManager.initialise (8, 2, savedAudio.get(), true);   // request up to 8 inputs / 2 outputs
    deviceManager.addChangeListener (this);

    // --- processor graph: build the fixed IO nodes + resident file player ---
    audioInNode  = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioInputNode));
    audioOutNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode));
    midiInNode   = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::midiInputNode));

    {
        auto fp = std::make_unique<FilePlayerProcessor>();
        filePlayer = fp.get();
        filePlayerNode = graph.addNode (std::move (fp));
    }

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    deviceManager.addMidiInputDeviceCallback ({}, &player);   // feed MIDI into graph (for VSTi)
    deviceManager.addMidiInputDeviceCallback ({}, this);      // our own thru-to-Reface path
    rebuildConnections();

    // === UI ===
    auto addBtn = [this] (juce::Button& b) { addAndMakeVisible (b); };
    addBtn (audioSettingsButton); addBtn (resetAudioButton);
    addBtn (loadButton); addBtn (editorButton);
    addBtn (clearButton);         addBtn (bypassButton); addBtn (midiThruButton);
    addBtn (loadInstButton);      addBtn (instEditorButton); addBtn (clearInstButton);
    addBtn (openMidiButton);
    addBtn (openFileButton);      addBtn (playButton); addBtn (stopButton); addBtn (loopButton);

    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    resetAudioButton.onClick    = [this]
    {
        // One-click recovery when the ASIO driver goes irregular (post-xrun).
        const bool wasPlaying = filePlayer->transport.isPlaying();
        filePlayer->transport.stop();
        deviceManager.closeAudioDevice();
        deviceManager.restartLastAudioDevice();
        rebuildConnections();
        if (wasPlaying) filePlayer->transport.start();
        setStatus ("Audio device restarted.");
    };
    loadButton.onClick          = [this] { loadPluginDialog (false); };
    loadInstButton.onClick      = [this] { loadPluginDialog (true); };
    editorButton.onClick        = [this] { toggleEditorFor (effectNode, currentEffectName, editorWindow); };
    instEditorButton.onClick    = [this] { toggleEditorFor (instrumentNode, currentInstrumentName, instEditorWindow); };
    clearButton.onClick         = [this] { removeEffect(); };
    clearInstButton.onClick     = [this] { removeInstrument(); };

    midiThruButton.setToggleState (true, juce::dontSendNotification);
    midiThruButton.onClick = [this] { midiThru = midiThruButton.getToggleState(); };
    bypassButton.onClick   = [this]
    {
        // In pre-render mode the live FX node stays bypassed (render thread owns the sound).
        if (effectNode != nullptr && ! preRenderActive())
            effectNode->setBypassed (bypassButton.getToggleState());
    };

    // --- source selector ---
    sourceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (sourceLabel);
    addAndMakeVisible (sourceCombo);
    sourceCombo.addItem ("Live input (audio in)", srcLive);
    sourceCombo.addItem ("VST instrument (MIDI)", srcInstrument);
    sourceCombo.addItem ("Audio file player",     srcFile);
    sourceCombo.setSelectedId (srcLive, juce::dontSendNotification);
    sourceCombo.onChange = [this]
    {
        if (currentSourceMode() != srcFile && preRenderButton.getToggleState())
            setPreRenderEnabled (false);   // pre-render only exists in file mode
        if (currentSourceMode() != srcFile && gpuFxButton.getToggleState())
            setGpuFxEnabled (false);       // GPU FX transforms the playable file
        applyBackendForMode (currentSourceMode());
        rebuildConnections();
        switch (currentSourceMode())
        {
            case srcInstrument: setStatus (instrumentNode != nullptr ? "Source: VSTi -> FX"
                                                                     : "Source: VSTi (load one first)"); break;
            case srcFile:       setStatus (filePlayer->hasFile() ? "Source: file player -> FX"
                                                                 : "Source: file player (open a file)"); break;
            default:            setStatus ("Source: live input -> FX"); break;
        }
    };

    // --- auto backend: keep the crash-prone ASIO driver out of the hot path
    //     whenever latency does not matter (file playback / pre-render). ---
    addAndMakeVisible (autoBackendButton);
    autoBackendButton.setTooltip ("Switch the audio backend with the source: WASAPI for file playback "
                                  "(latency is irrelevant there), ASIO only for live/VSTi.");
    autoBackendButton.setToggleState (autoBackendFile().existsAsFile()
                                          ? autoBackendFile().loadFileAsString().trim() == "1"
                                          : true,   // default ON
                                      juce::dontSendNotification);
    autoBackendButton.onClick = [this]
    {
        autoBackendFile().replaceWithText (autoBackendButton.getToggleState() ? "1" : "0");
        applyBackendForMode (currentSourceMode());
    };

    // --- file player transport ---
    playButton.onClick = [this]
    {
        auto& t = filePlayer->transport;
        if (! filePlayer->hasFile()) { setStatus ("No audio file loaded."); return; }
        if (t.isPlaying())
            t.stop();
        else
        {
            if (t.hasStreamFinished() || t.getCurrentPosition() >= t.getLengthInSeconds() - 0.05)
                t.setPosition (0.0);
            t.start();
        }
    };
    stopButton.onClick = [this]
    {
        filePlayer->transport.stop();
        filePlayer->transport.setPosition (0.0);
    };
    loopButton.onClick = [this]
    {
        filePlayer->setLooping (loopButton.getToggleState());
        if (cacheSource != nullptr)
            cacheSource->setLooping (loopButton.getToggleState());
    };
    openFileButton.onClick = [this] { openAudioFileDialog(); };

    addAndMakeVisible (preRenderButton);
    preRenderButton.setTooltip ("Render the file through the FX chain ahead of playback; the audio thread only streams RAM.");
    preRenderButton.onClick = [this] { setPreRenderEnabled (preRenderButton.getToggleState()); };

    renderLabel.setJustificationType (juce::Justification::centredLeft);
    renderLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    renderLabel.setText ("PRE-RENDER off", juce::dontSendNotification);
    addAndMakeVisible (renderLabel);

    // --- GPU FX (gpufx worker transforms the playable file) ---
    addAndMakeVisible (gpuFxButton);
    gpuFxButton.setTooltip ("Render the file through the gpufx worker (spectral rebuild on the GPU); "
                            "knob changes re-render and hot-swap at the playback position.");
    gpuFxButton.onClick = [this] { setGpuFxEnabled (gpuFxButton.getToggleState()); };

    gpuStatusLabel.setJustificationType (juce::Justification::centredLeft);
    gpuStatusLabel.setColour (juce::Label::textColourId, juce::Colours::mediumpurple);
    gpuStatusLabel.setText ("GPU FX off", juce::dontSendNotification);
    addAndMakeVisible (gpuStatusLabel);

    addAndMakeVisible (gpuViewport);
    gpuViewport.setViewedComponent (&gpuPanelHolder, false);
    gpuViewport.setScrollBarsShown (true, false);
    gpuViewport.setScrollBarThickness (12);

    juce::Component::SafePointer<MainComponent> weakThis (this);
    gpuWorker.onSchemaReady = [weakThis] (juce::var schema)
    {
        if (weakThis != nullptr)
            weakThis->buildGpuPanel (schema);
    };
    gpuWorker.onRenderDone = [weakThis] (bool ok, juce::String info, juce::File in, juce::File out)
    {
        if (weakThis != nullptr)
            weakThis->handleGpuRenderDone (ok, info, in, out);
    };

    posSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    posSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    posSlider.setRange (0.0, 1.0);
    posSlider.onValueChange = [this]
    {
        // Only fires for user gestures: the timer updates with dontSendNotification.
        if (filePlayer->hasFile())
            filePlayer->transport.setPosition (posSlider.getValue());
    };
    addAndMakeVisible (posSlider);

    timeLabel.setJustificationType (juce::Justification::centredRight);
    timeLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    timeLabel.setText ("--:-- / --:--", juce::dontSendNotification);
    addAndMakeVisible (timeLabel);

    fileLabel.setJustificationType (juce::Justification::centredLeft);
    fileLabel.setColour (juce::Label::textColourId, juce::Colours::khaki);
    fileLabel.setText ("No audio file loaded", juce::dontSendNotification);
    addAndMakeVisible (fileLabel);

    instLabel.setJustificationType (juce::Justification::centredLeft);
    instLabel.setColour (juce::Label::textColourId, juce::Colours::lightgreen);
    instLabel.setText ("No instrument loaded", juce::dontSendNotification);
    addAndMakeVisible (instLabel);

    // --- MIDI bounce (Phase C: MIDI file -> VSTi offline bounce -> file player) ---
    // One MIDI home next to the exe: recorded takes land there and the open
    // dialog starts there, so play -> auto-save -> bounce is one folder.
    midiFolder = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                     .getParentDirectory().getChildFile ("MIDI");
    midiFolder.createDirectory();
    if (! midiFolder.isDirectory())
    {
        midiFolder = appDir().getChildFile ("MIDI");   // exe dir not writable - unlikely here
        midiFolder.createDirectory();
    }
    midiRecorder.setDirectory (midiFolder);

    openMidiButton.setTooltip ("Bounce a MIDI file through the loaded VSTi (faster than realtime) "
                               "and play the result on the file player - GPU FX / FX / PRE-RENDER "
                               "all apply. VSTi knob changes re-bounce and hot-swap. "
                               "Opens in " + midiFolder.getFullPathName());
    openMidiButton.onClick = [this] { openMidiFileDialog(); };

    addAndMakeVisible (midiRecButton);
    midiRecButton.setTooltip ("Dashcam MIDI capture: a take opens on the first note-on and auto-saves "
                              "to " + midiFolder.getFullPathName() + " after "
                              + juce::String ((int) MidiTakeRecorder::idleCloseSeconds)
                              + " s of silence. Takes with fewer than "
                              + juce::String (MidiTakeRecorder::minNoteOns) + " notes are discarded.");
    midiRecButton.setToggleState (midiRecFile().existsAsFile()
                                      ? midiRecFile().loadFileAsString().trim() == "1"
                                      : true,   // default ON - noodling is never lost
                                  juce::dontSendNotification);
    midiRecorder.setEnabled (midiRecButton.getToggleState());
    midiRecButton.onClick = [this]
    {
        const bool on = midiRecButton.getToggleState();
        midiRecFile().replaceWithText (on ? "1" : "0");
        midiRecorder.setEnabled (on);
        if (! on)
        {
            const auto saved = midiRecorder.closeAndSave();   // keep what was played
            if (saved != juce::File())
                setStatus ("MIDI take saved: " + saved.getFileName());
        }
    };

    midiStatusLabel.setJustificationType (juce::Justification::centredLeft);
    midiStatusLabel.setColour (juce::Label::textColourId, juce::Colours::skyblue);
    midiStatusLabel.setText ("MIDI: none (bounces thru the VSTi to the file player)",
                             juce::dontSendNotification);
    addAndMakeVisible (midiStatusLabel);

    bounceEngine.onDone = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
                          (bool ok, juce::File out, juce::String info)
    {
        if (safeThis != nullptr)
            safeThis->handleBounceDone (ok, out, info);
    };

    for (auto* l : { &midiOutLabel, &inputPairLabel, &recentLabel })
    {
        l->setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (*l);
    }
    addAndMakeVisible (midiOutCombo);
    addAndMakeVisible (inputPairCombo);
    addAndMakeVisible (recentCombo);

    inputPairCombo.addItem ("1 / 2", 1);
    inputPairCombo.addItem ("3 / 4", 2);
    inputPairCombo.addItem ("5 / 6", 3);
    inputPairCombo.addItem ("7 / 8", 4);
    inputPairCombo.setSelectedId (2, juce::dontSendNotification);   // default 3/4
    inputPairCombo.onChange = [this]
    {
        inputPairStart = (inputPairCombo.getSelectedId() - 1) * 2;
        rebuildConnections();
    };

    midiOutCombo.onChange = [this]
    {
        const auto id = midiOutCombo.getSelectedId();
        if (id <= 0) { const juce::ScopedLock sl (midiOutLock); midiOut.reset(); return; }
        auto devices = juce::MidiOutput::getAvailableDevices();
        if (id - 1 < devices.size()) openMidiOut (devices[id - 1].identifier);
    };
    recentCombo.onChange = [this]
    {
        const int idx = recentCombo.getSelectedId() - 1;
        auto types = knownPlugins.getTypes();
        if (idx >= 0 && idx < types.size())
            loadPluginFromDescription (types[idx], types[idx].isInstrument);
    };

    pluginLabel.setJustificationType (juce::Justification::centredLeft);
    pluginLabel.setColour (juce::Label::textColourId, juce::Colours::aqua);
    addAndMakeVisible (pluginLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (statusLabel);

    refreshMidiOutList();
    refreshRecentList();
    setStatus ("Ready. Open Audio/MIDI Settings and pick UR-RT2 (ASIO).");
    pluginLabel.setText ("No FX loaded (source -> output monitor)", juce::dontSendNotification);

    // If the last session ended on the other backend (e.g. closed in file/WASAPI
    // mode), realign the device with the initial source mode.
    applyBackendForMode (currentSourceMode());

    startTimerHz (10);
    setSize (680, 846);   // room for the GPU FX panel (11 params) + MIDI bounce row
}

MainComponent::~MainComponent()
{
    stopTimer();
    gpuWorker.shutdown();
    renderEngine.stopRender();
    bounceEngine.stopBounce();
    if (effectNode != nullptr)
        effectNode->getProcessor()->removeListener (this);
    if (instrumentNode != nullptr)
        instrumentNode->getProcessor()->removeListener (this);
    editorWindow = nullptr;
    instEditorWindow = nullptr;
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeMidiInputDeviceCallback ({}, &player);
    midiRecorder.closeAndSave();   // an open take survives app close
    deviceManager.removeAudioCallback (&player);
    deviceManager.removeChangeListener (this);
    player.setProcessor (nullptr);

    saveBackendSnapshot();
    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (audioStateFile());

    const juce::ScopedLock sl (midiOutLock);
    midiOut.reset();
}

//==============================================================================
juce::File MainComponent::appDir() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("VstTestBench");
    dir.createDirectory();
    return dir;
}
juce::File MainComponent::cacheFile()      const { return appDir().getChildFile ("known_plugins.xml"); }
juce::File MainComponent::audioStateFile() const { return appDir().getChildFile ("audio_settings.xml"); }
juce::File MainComponent::lastDirFile()    const { return appDir().getChildFile ("last_audio_dir.txt"); }
juce::File MainComponent::midiRecFile()    const { return appDir().getChildFile ("midi_rec.txt"); }
juce::File MainComponent::autoBackendFile() const { return appDir().getChildFile ("auto_backend.txt"); }

juce::File MainComponent::backendStateFile (const juce::String& typeName) const
{
    const auto slug = typeName.replaceCharacter (' ', '_')
                              .retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
    return appDir().getChildFile ("audio_settings_" + slug + ".xml");
}

void MainComponent::saveBackendSnapshot()
{
    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (backendStateFile (deviceManager.getCurrentAudioDeviceType()));
}

//==============================================================================
// Latency only matters when the source is live; file playback streams a file
// (or the pre-render cache), so it can run on WASAPI and keep the ASIO driver
// untouched. Each backend's exact device setup is snapshotted so switching
// back restores it (ASIO input pair 3/4, buffer size, ...).
void MainComponent::applyBackendForMode (int mode)
{
    if (! autoBackendButton.getToggleState())
        return;

    const juce::String want = (mode == srcFile) ? "Windows Audio" : "ASIO";
    if (deviceManager.getCurrentAudioDeviceType() == want)
        return;

    bool available = false;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == want)
            { available = true; break; }

    if (! available)
    {
        setStatus ("Backend " + want + " not available - staying on "
                   + deviceManager.getCurrentAudioDeviceType());
        return;
    }

    saveBackendSnapshot();   // remember the outgoing backend's setup

    const bool wasPlaying = filePlayer->transport.isPlaying();
    filePlayer->transport.stop();

    std::unique_ptr<juce::XmlElement> snap (juce::XmlDocument::parse (backendStateFile (want)));
    if (snap != nullptr)
        deviceManager.initialise (8, 2, snap.get(), true);
    else
        deviceManager.setCurrentAudioDeviceType (want, true);   // first time: default device

    rebuildConnections();

    // The device (and possibly its sample rate) changed under the cache.
    if (preRenderActive())
        beginPreRender();

    if (wasPlaying)
        filePlayer->transport.start();

    auto* dev = deviceManager.getCurrentAudioDevice();
    setStatus ("Audio backend -> " + want
               + (dev != nullptr ? " (" + dev->getName() + ")" : juce::String (" - no device opened!")));
}

juce::AudioPluginFormat* MainComponent::vst3Format() const
{
    for (auto* f : formatManager.getFormats())
        if (f->getName().containsIgnoreCase ("VST3"))
            return f;
    return nullptr;
}

int MainComponent::currentSourceMode() const
{
    const int id = sourceCombo.getSelectedId();
    return id > 0 ? id : (int) srcLive;
}

void MainComponent::setStatus (const juce::String& s)
{
    statusLabel.setText (s, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::loadKnownPlugins()
{
    if (auto xml = juce::XmlDocument::parse (cacheFile()))
        knownPlugins.recreateFromXml (*xml);
}

void MainComponent::saveKnownPlugins()
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo (cacheFile());
}

void MainComponent::refreshRecentList()
{
    recentCombo.clear (juce::dontSendNotification);
    auto types = knownPlugins.getTypes();
    for (int i = 0; i < types.size(); ++i)
        recentCombo.addItem ((types[i].isInstrument ? "[inst] " : "[fx] ") + types[i].name, i + 1);
    recentCombo.setTextWhenNothingSelected (types.isEmpty() ? "(none cached yet)"
                                                            : "select cached plugin...");
}

void MainComponent::refreshMidiOutList()
{
    midiOutCombo.clear (juce::dontSendNotification);
    midiOutCombo.addItem ("(none)", -1);
    auto devices = juce::MidiOutput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
    {
        midiOutCombo.addItem (devices[i].name, i + 1);
        if (devices[i].name.containsIgnoreCase ("reface")
            || devices[i].name.containsIgnoreCase ("cp"))
            midiOutCombo.setSelectedId (i + 1, juce::sendNotificationSync);
    }
    if (midiOutCombo.getSelectedId() == 0)
        midiOutCombo.setSelectedId (-1, juce::dontSendNotification);
}

void MainComponent::openMidiOut (const juce::String& identifier)
{
    auto opened = juce::MidiOutput::openDevice (identifier);
    const juce::ScopedLock sl (midiOutLock);
    midiOut = std::move (opened);
    setStatus (midiOut != nullptr ? "Reface MIDI out opened: " + midiOut->getName()
                                  : "Failed to open MIDI out.");
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        deviceManager,
        0, 8,      // min / max input channels
        0, 2,      // min / max output channels
        true,      // show MIDI inputs
        false,     // show MIDI outputs (we handle Reface via our own combo)
        false,     // channels as stereo pairs -> false so 3/4 are individually selectable
        false);    // hide advanced
    selector->setSize (450, 480);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector.release());
    o.dialogTitle = "Audio / MIDI Settings";
    o.dialogBackgroundColour = juce::Colours::darkgrey;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

//==============================================================================
void MainComponent::loadPluginDialog (bool asInstrument)
{
    auto chooser = std::make_shared<juce::FileChooser> (
        asInstrument ? "Select a VST3 instrument" : "Select a VST3 effect",
        juce::File ("C:/Program Files/Common Files/VST3"),
        "*.vst3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectDirectories,
        [this, chooser, asInstrument] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;

            auto* fmt = vst3Format();
            if (fmt == nullptr) { setStatus ("VST3 format not available."); return; }

            juce::OwnedArray<juce::PluginDescription> found;
            setStatus ("Scanning " + file.getFileName() + " ...");
            knownPlugins.scanAndAddFile (file.getFullPathName(), true, found, *fmt);
            saveKnownPlugins();
            refreshRecentList();

            if (found.isEmpty())
                setStatus ("No plugin found in " + file.getFileName());
            else
                loadPluginFromDescription (*found.getFirst(), asInstrument);
        });
}

void MainComponent::loadPluginFromDescription (const juce::PluginDescription& desc, bool asInstrument)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    const double rate  = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    const int    block = setup.bufferSize  > 0 ? setup.bufferSize  : 512;

    setStatus ("Loading " + desc.name + " ...");
    formatManager.createPluginInstanceAsync (
        desc, rate, block,
        [this, desc, asInstrument] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (instance == nullptr)
            {
                setStatus ("Load failed: " + error);
                return;
            }
            if (asInstrument)
                setInstrumentNode (std::move (instance), desc);
            else
                setEffectNode (std::move (instance), desc);
        });
}

static void configureInstance (juce::AudioPluginInstance& instance,
                               const juce::AudioDeviceManager& dm)
{
    instance.enableAllBuses();
    auto setup = dm.getAudioDeviceSetup();
    instance.setPlayConfigDetails (instance.getTotalNumInputChannels(),
                                   instance.getTotalNumOutputChannels(),
                                   setup.sampleRate > 0 ? setup.sampleRate : 48000.0,
                                   setup.bufferSize  > 0 ? setup.bufferSize  : 512);
}

void MainComponent::setEffectNode (std::unique_ptr<juce::AudioPluginInstance> instance,
                                   const juce::PluginDescription& desc)
{
    editorWindow = nullptr;
    if (effectNode != nullptr)
    {
        effectNode->getProcessor()->removeListener (this);
        graph.removeNode (effectNode->nodeID);
    }

    renderEngine.stopRender();
    offlineFx.reset();          // clone of the previous FX, now stale
    currentFxDesc = desc;

    configureInstance (*instance, deviceManager);
    effectNode = graph.addNode (std::move (instance));
    effectNode->setBypassed (preRenderActive() || bypassButton.getToggleState());
    effectNode->getProcessor()->addListener (this);
    currentEffectName = desc.name;

    rebuildConnections();

    if (preRenderActive())
        createOfflineFx();      // re-render kicks off once the clone is ready
    pluginLabel.setText ("FX: " + desc.name + "  ("
                         + juce::String (effectNode->getProcessor()->getTotalNumInputChannels()) + " in / "
                         + juce::String (effectNode->getProcessor()->getTotalNumOutputChannels()) + " out)",
                         juce::dontSendNotification);
    setStatus ("Loaded FX " + desc.name);
}

void MainComponent::setInstrumentNode (std::unique_ptr<juce::AudioPluginInstance> instance,
                                       const juce::PluginDescription& desc)
{
    instEditorWindow = nullptr;
    bounceEngine.stopBounce();
    offlineInst.reset();          // clone of the previous instrument, now stale
    if (instrumentNode != nullptr)
    {
        instrumentNode->getProcessor()->removeListener (this);
        graph.removeNode (instrumentNode->nodeID);
    }

    currentInstDesc = desc;
    configureInstance (*instance, deviceManager);
    instrumentNode = graph.addNode (std::move (instance));
    instrumentNode->getProcessor()->addListener (this);   // knob changes re-bounce the MIDI chain
    instrumentProc = instrumentNode->getProcessor();
    currentInstrumentName = desc.name;

    // Loading an instrument implies wanting to hear it: switch the source stage.
    sourceCombo.setSelectedId (srcInstrument, juce::dontSendNotification);
    applyBackendForMode (srcInstrument);

    rebuildConnections();
    instLabel.setText ("Inst: " + desc.name + "  ("
                       + juce::String (instrumentNode->getProcessor()->getTotalNumOutputChannels()) + " out)",
                       juce::dontSendNotification);
    setStatus ("Loaded instrument " + desc.name + " (source switched to VSTi)");

    // A different instrument means the bounced wav no longer matches the chain.
    if (midiChainActive())
    {
        instStale = true;
        lastInstChangeMs = juce::Time::getMillisecondCounter();
    }
}

void MainComponent::removeEffect()
{
    editorWindow = nullptr;
    renderEngine.stopRender();
    offlineFx.reset();
    if (effectNode != nullptr)
    {
        effectNode->getProcessor()->removeListener (this);
        graph.removeNode (effectNode->nodeID);
        effectNode = nullptr;
    }
    currentEffectName = {};
    rebuildConnections();
    pluginLabel.setText ("No FX loaded (source -> output monitor)", juce::dontSendNotification);
    setStatus ("FX removed.");

    if (preRenderActive())
        syncOfflineStateAndRender();   // re-render dry
}

void MainComponent::removeInstrument()
{
    instEditorWindow = nullptr;
    bounceEngine.stopBounce();
    offlineInst.reset();
    instrumentProc = nullptr;
    if (instrumentNode != nullptr)
    {
        instrumentNode->getProcessor()->removeListener (this);
        graph.removeNode (instrumentNode->nodeID);
        instrumentNode = nullptr;
    }
    currentInstrumentName = {};
    rebuildConnections();
    instLabel.setText ("No instrument loaded", juce::dontSendNotification);
    setStatus ("Instrument removed.");
}

//==============================================================================
void MainComponent::rebuildConnections()
{
    for (auto c : graph.getConnections())
        graph.removeConnection (c);

    const int outChans = 2;
    const int mode = currentSourceMode();

    // --- pick the source node feeding the FX stage ---
    Graph::Node::Ptr src;
    int srcChanBase = 0;
    int srcChans    = 2;

    if (mode == srcInstrument && instrumentNode != nullptr)
    {
        src = instrumentNode;
        srcChans = instrumentNode->getProcessor()->getTotalNumOutputChannels();
    }
    else if (mode == srcFile)
    {
        src = filePlayerNode;
    }
    else   // live input (also the fallback when no instrument is loaded)
    {
        src = audioInNode;
        srcChanBase = inputPairStart;
    }

    // MIDI in -> instrument (whenever one is loaded, regardless of mode)
    if (instrumentNode != nullptr)
        graph.addConnection ({ { midiInNode->nodeID,     Graph::midiChannelIndex },
                               { instrumentNode->nodeID, Graph::midiChannelIndex } });

    if (effectNode != nullptr && ! preRenderActive())
    {
        auto* proc = effectNode->getProcessor();
        const int pin  = juce::jmax (0, proc->getTotalNumInputChannels());
        const int pout = juce::jmax (0, proc->getTotalNumOutputChannels());

        // source -> FX inputs
        for (int ch = 0; ch < juce::jmin (2, srcChans, pin); ++ch)
            graph.addConnection ({ { src->nodeID,        srcChanBase + ch },
                                   { effectNode->nodeID, ch } });

        // FX outputs -> device out
        for (int ch = 0; ch < juce::jmin (outChans, pout); ++ch)
            graph.addConnection ({ { effectNode->nodeID,  ch },
                                   { audioOutNode->nodeID, ch } });

        // MIDI in -> FX (for MIDI-controlled effects)
        graph.addConnection ({ { midiInNode->nodeID, Graph::midiChannelIndex },
                               { effectNode->nodeID, Graph::midiChannelIndex } });
    }
    else
    {
        // No FX (or pre-render mode: FX already baked into the cache) ->
        // stream the source straight to the outputs.
        for (int ch = 0; ch < juce::jmin (outChans, srcChans); ++ch)
            graph.addConnection ({ { src->nodeID,         srcChanBase + ch },
                                   { audioOutNode->nodeID, ch } });
    }
}

//==============================================================================
void MainComponent::openAudioFileDialog()
{
    juce::File startDir (lastDirFile().loadFileAsString().trim());
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    auto chooser = std::make_shared<juce::FileChooser> (
        "Select an audio file",
        startDir,
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.opus;*.m4a;*.aac;*.alac;*.wma;*.caf");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;
            lastDirFile().replaceWithText (file.getParentDirectory().getFullPathName());
            loadAudioFile (file);
        });
}

void MainComponent::loadAudioFile (const juce::File& file)
{
    abandonMidiChain();   // opening a plain audio file replaces the MIDI chain
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (file));
    if (reader != nullptr)
    {
        finishAudioFileLoad (std::move (reader), file, file);
        return;
    }
    // Not natively decodable (opus / m4a / alac ...) -> ffmpeg fallback.
    setStatus ("No native decoder for " + file.getFileExtension() + " -> converting via ffmpeg ...");
    convertWithFfmpegAsync (file);
}

void MainComponent::convertWithFfmpegAsync (const juce::File& source)
{
    const auto dest = appDir().getChildFile ("decode_cache.wav");
    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::Thread::launch ([safeThis, source, dest]
    {
        dest.deleteFile();
        juce::ChildProcess proc;
        juce::StringArray args { "ffmpeg", "-y", "-i", source.getFullPathName(),
                                 "-acodec", "pcm_f32le", dest.getFullPathName() };
        juce::String error;
        if (! proc.start (args, 0))
            error = "ffmpeg not found on PATH. Install ffmpeg or use wav/flac/mp3/ogg.";
        else if (! proc.waitForProcessToFinish (120000) || ! dest.existsAsFile())
            error = "ffmpeg conversion failed for " + source.getFileName();

        juce::MessageManager::callAsync ([safeThis, source, dest, error]
        {
            if (safeThis == nullptr) return;
            if (error.isNotEmpty()) { safeThis->setStatus (error); return; }

            std::unique_ptr<juce::AudioFormatReader> reader (
                safeThis->audioFormats.createReaderFor (dest));
            if (reader == nullptr) { safeThis->setStatus ("Could not read converted wav."); return; }
            safeThis->finishAudioFileLoad (std::move (reader), source, dest);
        });
    });
}

void MainComponent::finishAudioFileLoad (std::unique_ptr<juce::AudioFormatReader> reader,
                                         const juce::File& original, const juce::File& readable)
{
    renderEngine.stopRender();   // a running render targets the previous file

    const double srcRate  = reader->sampleRate;
    const int    channels = (int) reader->numChannels;
    fileLengthSamples   = reader->lengthInSamples;
    fileSampleRate      = srcRate;
    currentOriginalFile = original;
    currentPlayableFile = readable;

    if (! filePlayer->loadReader (std::move (reader), loopButton.getToggleState()))
    {
        setStatus ("Failed to load " + original.getFileName());
        return;
    }

    const double len = filePlayer->transport.getLengthInSeconds();
    posSlider.setRange (0.0, juce::jmax (0.01, len));
    posSlider.setValue (0.0, juce::dontSendNotification);

    fileLabel.setText (original.getFileName()
                       + "  (" + juce::String (srcRate / 1000.0, 1) + " kHz, "
                       + juce::String (channels) + " ch, " + formatTime (len) + ")",
                       juce::dontSendNotification);

    // Opening a file implies wanting to hear it: switch the source stage.
    sourceCombo.setSelectedId (srcFile, juce::dontSendNotification);
    applyBackendForMode (srcFile);
    rebuildConnections();
    setStatus ("Loaded " + original.getFileName() + " (source switched to file player)");

    // The old GPU render belongs to the previous file: play dry until the
    // worker has baked the new one, then hot-swap.
    currentGpuFile = juce::File();
    if (gpuFxButton.getToggleState())
        requestGpuRender();

    if (preRenderButton.getToggleState())
        beginPreRender();   // new file -> new cache
}

juce::String MainComponent::formatTime (double seconds) const
{
    const int total = (int) std::floor (juce::jmax (0.0, seconds));
    return juce::String::formatted ("%d:%02d", total / 60, total % 60);
}

//==============================================================================
// MIDI file -> VSTi offline bounce (Phase C). The bounce result enters the
// normal file-player path, so GPU FX / VST FX / PRE-RENDER apply unchanged.
bool MainComponent::midiChainActive() const
{
    return currentMidiFile != juce::File() && currentOriginalFile == currentMidiFile;
}

void MainComponent::abandonMidiChain()
{
    if (currentMidiFile == juce::File() && ! bounceEngine.isBouncing())
        return;
    bounceEngine.stopBounce();
    currentMidiFile = juce::File();
    firstBouncePending = false;
    instStale = false;
    midiReadyText = "MIDI: none";
    midiStatusLabel.setText ("MIDI: none (bounces thru the VSTi to the file player)",
                             juce::dontSendNotification);
}

void MainComponent::openMidiFileDialog()
{
    if (instrumentNode == nullptr)
    {
        setStatus ("Load a VSTi first - the MIDI file is bounced through it.");
        return;
    }

    // Same folder the take recorder saves into: play -> auto-save -> bounce.
    auto chooser = std::make_shared<juce::FileChooser> ("Select a MIDI file", midiFolder,
                                                        "*.mid;*.midi;*.smf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;
            loadMidiFile (file);
        });
}

void MainComponent::loadMidiFile (const juce::File& file)
{
    juce::FileInputStream in (file);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in))
    {
        setStatus ("Could not read MIDI file " + file.getFileName());
        return;
    }
    mf.convertTimestampTicksToSeconds();   // applies the tempo map

    juce::MidiMessageSequence merged;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        merged.addSequence (*mf.getTrack (t), 0.0);
    merged.sort();

    if (merged.getNumEvents() == 0)
    {
        setStatus ("No events in " + file.getFileName());
        return;
    }

    bounceEngine.stopBounce();
    midiSequence       = std::move (merged);
    currentMidiFile    = file;
    firstBouncePending = true;
    instStale          = false;
    midiReadyText = "MIDI: " + file.getFileName();
    midiStatusLabel.setText ("MIDI: bouncing " + file.getFileName() + " thru "
                             + currentInstrumentName + " ...", juce::dontSendNotification);
    syncInstStateAndBounce();
}

void MainComponent::createOfflineInstAndBounce()
{
    offlineInstLoading = true;
    setStatus ("Creating offline VSTi instance ...");
    auto setup = deviceManager.getAudioDeviceSetup();
    formatManager.createPluginInstanceAsync (
        currentInstDesc,
        setup.sampleRate > 0 ? setup.sampleRate : 48000.0, 4096,
        [this] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            offlineInstLoading = false;
            if (instance == nullptr)
            {
                firstBouncePending = false;
                midiStatusLabel.setText ("MIDI: offline VSTi load failed", juce::dontSendNotification);
                setStatus ("Offline VSTi load failed: " + error);
                return;
            }
            instance->enableAllBuses();
            offlineInst = std::move (instance);
            if (currentMidiFile != juce::File())
                syncInstStateAndBounce();
        });
}

void MainComponent::syncInstStateAndBounce()
{
    if (instrumentNode == nullptr || currentMidiFile == juce::File() || bounceEngine.isBouncing())
        return;
    if (offlineInst == nullptr)
    {
        if (! offlineInstLoading)
            createOfflineInstAndBounce();   // bounce starts when the clone arrives
        return;
    }

    juce::MemoryBlock state;
    instrumentNode->getProcessor()->getStateInformation (state);
    if (state.getSize() > 0)
        offlineInst->setStateInformation (state.getData(), (int) state.getSize());

    // Same JUCE-VST3 wrapper trap as the offline FX clone: a bypass PARAMETER
    // may have travelled inside the state copy. Clear it explicitly.
    if (auto* bypass = offlineInst->getBypassParameter())
        bypass->setValueNotifyingHost (0.0f);

    auto setup = deviceManager.getAudioDeviceSetup();
    ++bounceGeneration;
    const auto out = appDir().getChildFile (juce::String::formatted ("midi_bounce_%04d.wav",
                                                                     bounceGeneration));
    bounceEngine.startBounce (midiSequence, offlineInst.get(),
                              setup.sampleRate > 0 ? setup.sampleRate : 48000.0, out);
}

void MainComponent::handleBounceDone (bool ok, juce::File out, juce::String info)
{
    if (currentMidiFile == juce::File())   // chain abandoned while the bounce ran
    {
        out.deleteFile();
        return;
    }
    if (! ok)
    {
        out.deleteFile();
        firstBouncePending = false;
        midiReadyText = "MIDI bounce failed: " + info;
        midiStatusLabel.setText (midiReadyText, juce::dontSendNotification);
        setStatus ("MIDI bounce failed: " + info);
        return;
    }

    midiReadyText = "MIDI: " + currentMidiFile.getFileName() + " - bounce ready (" + info + ")";
    auto isBounceFile = [] (const juce::File& f) { return f.getFileName().startsWith ("midi_bounce_"); };

    if (firstBouncePending)
    {
        firstBouncePending = false;
        std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (out));
        if (reader == nullptr)
        {
            midiStatusLabel.setText ("MIDI: could not read bounced wav", juce::dontSendNotification);
            return;
        }
        const juce::File oldWav = currentPlayableFile;
        // original = the .mid file: the file label names it and midiChainActive() keys on it.
        finishAudioFileLoad (std::move (reader), currentMidiFile, out);
        if (oldWav != out && isBounceFile (oldWav))
            oldWav.deleteFile();
        midiStatusLabel.setText (midiReadyText, juce::dontSendNotification);
        return;
    }

    // Knob re-bounce: hot-swap at the playback position (GPU FX flow's sibling).
    std::unique_ptr<juce::AudioFormatReader> probe (audioFormats.createReaderFor (out));
    if (probe == nullptr)
    {
        midiStatusLabel.setText ("MIDI: could not read bounced wav", juce::dontSendNotification);
        return;
    }
    const juce::int64 newLen  = probe->lengthInSamples;
    const double      newRate = probe->sampleRate;
    probe.reset();

    const juce::File oldWav     = currentPlayableFile;
    const bool       lenChanged = newLen != fileLengthSamples;
    currentPlayableFile = out;
    fileLengthSamples   = newLen;
    fileSampleRate      = newRate;

    if (gpuFxButton.getToggleState())
    {
        // The audible file is the GPU generation: keep playing it and re-render
        // from the new bounce; handleGpuRenderDone hot-swaps when it lands.
        requestGpuRender();
    }
    else
    {
        swapPlayableFilePreservingPosition (out);
        posSlider.setRange (0.0, juce::jmax (0.01, filePlayer->transport.getLengthInSeconds()));
        if (preRenderActive())
        {
            if (lenChanged)
                beginPreRender();              // cache must be re-sized
            else
                syncOfflineStateAndRender();   // re-lay cursor-first from the new bounce
        }
    }

    if (oldWav != out && isBounceFile (oldWav))
        oldWav.deleteFile();   // may fail while a stale GPU render still reads it - harmless
    midiStatusLabel.setText (midiReadyText, juce::dontSendNotification);
}

//==============================================================================
bool MainComponent::preRenderActive() const
{
    return preRenderButton.getToggleState() && currentSourceMode() == srcFile;
}

void MainComponent::setPreRenderEnabled (bool shouldEnable)
{
    if (shouldEnable)
    {
        if (currentSourceMode() != srcFile || ! filePlayer->hasFile())
        {
            setStatus ("PRE-RENDER needs the file player as source with a file loaded.");
            preRenderButton.setToggleState (false, juce::dontSendNotification);
            return;
        }
        preRenderButton.setToggleState (true, juce::dontSendNotification);
        beginPreRender();
    }
    else
    {
        preRenderButton.setToggleState (false, juce::dontSendNotification);
        renderEngine.stopRender();
        filePlayer->reattachReader();
        if (effectNode != nullptr)
            effectNode->setBypassed (bypassButton.getToggleState());
        rebuildConnections();
        renderLabel.setText ("PRE-RENDER off", juce::dontSendNotification);
        setStatus ("PRE-RENDER off: FX back in the live path.");
    }
}

void MainComponent::beginPreRender()
{
    renderEngine.stopRender();

    // 150M samples @48k stereo float ~= 52 min / 1.2 GB RAM - a sane ceiling.
    if (fileLengthSamples <= 0 || fileLengthSamples > 150'000'000)
    {
        setStatus ("File too long for PRE-RENDER cache (or no file).");
        preRenderButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    // Safe to (re)allocate here: the transport is currently on the reader
    // source (fresh load or toggle-on), so nothing reads the cache buffer.
    renderCache.primed.store (false);   // toggle-on always starts with a deterministic full render
    renderCache.valid.store (0);
    renderCache.length     = fileLengthSamples;
    renderCache.sampleRate = fileSampleRate;
    renderCache.data.setSize (2, (int) fileLengthSamples, false, true, true);

    if (cacheSource == nullptr)
        cacheSource = std::make_unique<CacheAudioSource> (renderCache);
    cacheSource->setLooping (loopButton.getToggleState());

    filePlayer->attachExternalSource (cacheSource.get(), fileSampleRate);
    if (effectNode != nullptr)
        effectNode->setBypassed (true);   // live node parked; render thread owns the sound
    rebuildConnections();

    if (effectNode != nullptr && offlineFx == nullptr)
    {
        if (! offlineFxLoading)
            createOfflineFx();            // render starts when the clone arrives
    }
    else
        syncOfflineStateAndRender();
}

void MainComponent::createOfflineFx()
{
    offlineFxLoading = true;
    setStatus ("Creating offline FX instance ...");
    formatManager.createPluginInstanceAsync (
        currentFxDesc,
        fileSampleRate > 0 ? fileSampleRate : 48000.0, 4096,
        [this] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            offlineFxLoading = false;
            if (instance == nullptr)
            {
                setStatus ("Offline FX load failed: " + error);
                setPreRenderEnabled (false);
                return;
            }
            offlineFx = std::move (instance);
            if (preRenderActive())
                syncOfflineStateAndRender();
        });
}

void MainComponent::syncOfflineStateAndRender (bool quickOnly)
{
    if (effectNode != nullptr && offlineFx != nullptr)
    {
        juce::MemoryBlock state;
        effectNode->getProcessor()->getStateInformation (state);
        if (state.getSize() > 0)
            offlineFx->setStateInformation (state.getData(), (int) state.getSize());

        // Node::setBypassed(true) on the parked live node maps to a real
        // bypass PARAMETER for JUCE-built VST3s (the wrapper auto-adds one),
        // and that just travelled into the clone via the state copy - so the
        // clone would render a bit-perfect dry pass. Clear it explicitly.
        // (Most commercial plugins expose no bypass parameter, which is why
        // they were unaffected.)
        if (auto* bypass = offlineFx->getBypassParameter())
            bypass->setValueNotifyingHost (0.0f);
    }

    fxStale = false;

    // After the first full render, re-renders target a SWITCH POINT slightly
    // ahead of the playhead (HLS-style segment switch): by the time playback
    // arrives there, the new sound is already laid down - a deterministic,
    // sample-aligned transition instead of a writer-overtakes-reader race.
    // Quick renders only refresh a short window past the switch point; the
    // deferred full render (janitor) sweeps the file once the knobs go quiet.
    juce::int64 startSample = 0, quickWindow = 0;
    if (renderCache.primed.load())
    {
        const double curSec = filePlayer->transport.getCurrentPosition();
        double leadSec = 0.0;
        if (filePlayer->transport.isPlaying())
        {
            // Enough lead to burn the 2s warm-up before the playhead arrives,
            // scaled by the measured render speed (0.5s fast .. 3s heavy).
            leadSec = juce::jlimit (0.5, 3.0, 1.5 * 2.0 / juce::jmax (1.0f, lastRenderSpeed));
        }
        startSample = (juce::int64) ((curSec + leadSec) * fileSampleRate);
        if (startSample >= fileLengthSamples)
            startSample = loopButton.getToggleState() ? startSample - fileLengthSamples
                                                      : fileLengthSamples;
        if (quickOnly)
            quickWindow = (juce::int64) (25.0 * fileSampleRate);
    }

    renderEngine.startRender (effectivePlayableFile(), audioFormats,
                              effectNode != nullptr ? offlineFx.get() : nullptr,
                              renderCache, startSample, quickWindow);
}

//==============================================================================
juce::File MainComponent::effectivePlayableFile() const
{
    // What the pre-render engine (and, via swap, the file player) should read:
    // the GPU-baked generation file when GPU FX is active, else the plain file.
    if (gpuFxButton.getToggleState() && currentGpuFile.existsAsFile())
        return currentGpuFile;
    return currentPlayableFile;
}

bool MainComponent::swapPlayableFilePreservingPosition (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (f));
    if (reader == nullptr)
    {
        setStatus ("Could not read " + f.getFileName());
        return false;
    }

    auto& t = filePlayer->transport;
    const bool   usingExternal = filePlayer->isUsingExternalSource();
    const double pos           = t.getCurrentPosition();
    const bool   playing       = t.isPlaying();

    if (! filePlayer->loadReader (std::move (reader), loopButton.getToggleState()))
        return false;

    // In PRE-RENDER mode the audible sound comes from the cache: put the
    // transport back on it, the fresh reader waits for pre-render-off.
    if (usingExternal && cacheSource != nullptr)
        filePlayer->attachExternalSource (cacheSource.get(), fileSampleRate);

    t.setPosition (pos);
    if (playing)
        t.start();
    return true;
}

void MainComponent::setGpuFxEnabled (bool shouldEnable)
{
    if (shouldEnable)
    {
        if (currentSourceMode() != srcFile || ! filePlayer->hasFile())
        {
            setStatus ("GPU FX needs the file player as source with a file loaded.");
            gpuFxButton.setToggleState (false, juce::dontSendNotification);
            return;
        }
        gpuFxButton.setToggleState (true, juce::dontSendNotification);
        gpuWorker.start();
        requestGpuRender();
    }
    else
    {
        gpuFxButton.setToggleState (false, juce::dontSendNotification);
        const juce::File old = currentGpuFile;
        currentGpuFile = juce::File();
        gpuDirty = false;

        if (filePlayer->hasFile() && old != juce::File())
            swapPlayableFilePreservingPosition (currentPlayableFile);
        old.deleteFile();

        if (preRenderActive())
            syncOfflineStateAndRender();   // cache back to the dry source
        gpuStatusLabel.setText ("GPU FX off", juce::dontSendNotification);
        setStatus ("GPU FX off: playing the plain file.");
    }
}

void MainComponent::requestGpuRender()
{
    if (! filePlayer->hasFile() || currentPlayableFile == juce::File())
        return;
    gpuWorker.start();
    ++gpuGeneration;
    const auto out = appDir().getChildFile (juce::String::formatted ("gpu_cache_%04d.wav", gpuGeneration));
    gpuWorker.requestRender (currentPlayableFile, out, collectGpuParams());
    gpuStatusLabel.setText ("GPU: rendering...", juce::dontSendNotification);
}

void MainComponent::handleGpuRenderDone (bool ok, juce::String info, juce::File in, juce::File out)
{
    if (! gpuFxButton.getToggleState())      // disabled while the render ran
    {
        out.deleteFile();
        return;
    }
    if (! ok)
    {
        gpuReadyText = "GPU render failed: " + info;
        gpuStatusLabel.setText (gpuReadyText, juce::dontSendNotification);
        return;
    }
    if (in != currentPlayableFile)           // a different file was loaded meanwhile
    {
        out.deleteFile();
        requestGpuRender();
        return;
    }

    const juce::File old = currentGpuFile;
    if (swapPlayableFilePreservingPosition (out))
    {
        currentGpuFile = out;
        if (old != juce::File() && old != out)
            old.deleteFile();                // reader released it in the swap

        gpuReadyText = "GPU ready (" + info + ")";
        gpuStatusLabel.setText (gpuReadyText, juce::dontSendNotification);

        // The cache holds the previous sound; re-lay it cursor-first so the
        // new render fades in at the switch point (existing mechanism).
        if (preRenderActive())
            syncOfflineStateAndRender();
    }
    else
    {
        out.deleteFile();
        gpuReadyText = "GPU: could not open rendered file";
        gpuStatusLabel.setText (gpuReadyText, juce::dontSendNotification);
    }
}

void MainComponent::buildGpuPanel (const juce::var& describeResponse)
{
    gpuModules.clear();

    const auto modules = describeResponse["modules"];
    if (! modules.isArray() || modules.size() == 0)
        return;

    auto markDirty = [this]
    {
        gpuDirty = true;
        lastGpuChangeMs = juce::Time::getMillisecondCounter();
    };

    for (const auto& m : *modules.getArray())
    {
        GpuModule gm;
        gm.name = m["name"].toString();

        gm.enable = std::make_unique<juce::ToggleButton> ("[" + m["label"].toString() + "]");
        gm.enable->setToggleState ((bool) m.getProperty ("default_enabled", true),
                                   juce::dontSendNotification);
        gm.enable->setColour (juce::ToggleButton::textColourId, juce::Colours::mediumpurple);
        gm.enable->onClick = markDirty;
        gpuPanelHolder.addAndMakeVisible (*gm.enable);

        const auto params = m["params"];
        if (params.isArray())
        {
            for (const auto& p : *params.getArray())
            {
                GpuControl c;
                c.name = p["name"].toString();
                c.type = p["type"].toString();
                const auto label = p["label"].toString();

                if (c.type == "bool")
                {
                    c.toggle = std::make_unique<juce::ToggleButton> (label);
                    c.toggle->setToggleState ((bool) p["default"], juce::dontSendNotification);
                    c.toggle->onClick = markDirty;
                    gpuPanelHolder.addAndMakeVisible (*c.toggle);
                }
                else if (c.type == "choice")
                {
                    c.label = std::make_unique<juce::Label> (juce::String(), label);
                    c.label->setFont (juce::FontOptions (12.0f));
                    c.label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
                    gpuPanelHolder.addAndMakeVisible (*c.label);

                    c.combo = std::make_unique<juce::ComboBox>();
                    const auto options = p["options"];
                    if (options.isArray())
                        for (int i = 0; i < options.size(); ++i)
                            c.combo->addItem (options[i].toString(), i + 1);
                    const auto def = p["default"].toString();
                    for (int i = 1; i <= c.combo->getNumItems(); ++i)
                        if (c.combo->getItemText (i - 1) == def)
                            { c.combo->setSelectedId (i, juce::dontSendNotification); break; }
                    if (c.combo->getSelectedId() == 0 && c.combo->getNumItems() > 0)
                        c.combo->setSelectedId (1, juce::dontSendNotification);
                    c.combo->onChange = markDirty;
                    gpuPanelHolder.addAndMakeVisible (*c.combo);
                }
                else   // float / int
                {
                    c.label = std::make_unique<juce::Label> (juce::String(), label);
                    c.label->setFont (juce::FontOptions (12.0f));
                    c.label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
                    gpuPanelHolder.addAndMakeVisible (*c.label);

                    c.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                               juce::Slider::TextBoxRight);
                    c.slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
                    c.slider->setRange ((double) p.getProperty ("min", 0.0),
                                        (double) p.getProperty ("max", 1.0),
                                        (double) p.getProperty ("step", 0.0));
                    c.slider->setValue ((double) p["default"], juce::dontSendNotification);
                    c.slider->onValueChange = markDirty;
                    gpuPanelHolder.addAndMakeVisible (*c.slider);
                }
                gm.controls.push_back (std::move (c));
            }
        }
        gpuModules.push_back (std::move (gm));
    }

    // The panel lives in a viewport capped at 340 px: five modules of chain
    // outgrow any window, so the chain scrolls instead of the app growing.
    setSize (680, juce::jmax (846, 690 + juce::jmin (340, gpuPanelContentHeight())));
}

int MainComponent::gpuPanelContentHeight() const
{
    int h = 0;
    for (const auto& gm : gpuModules)
        h += 28 + ((int) gm.controls.size() + 1) / 2 * 26 + 6;
    return h;
}

juce::var MainComponent::collectGpuParams() const
{
    // Module chain for the worker: enabled modules in describe order.
    juce::Array<juce::var> chain;
    for (const auto& gm : gpuModules)
    {
        if (gm.enable == nullptr || ! gm.enable->getToggleState())
            continue;
        auto* params = new juce::DynamicObject();
        for (const auto& c : gm.controls)
            params->setProperty (c.name,
                c.type == "bool"   ? juce::var (c.toggle->getToggleState())
              : c.type == "choice" ? juce::var (c.combo->getText())
                                   : juce::var (c.slider->getValue()));
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("module", gm.name);
        entry->setProperty ("params", juce::var (params));
        chain.add (juce::var (entry));
    }
    return juce::var (chain);
}

//==============================================================================
void MainComponent::audioProcessorParameterChanged (juce::AudioProcessor* proc, int, float)
{
    // May arrive on any thread: only touch atomics.
    if (proc != nullptr && proc == instrumentProc.load())
    {
        instStale = true;
        lastInstChangeMs = juce::Time::getMillisecondCounter();
    }
    else
    {
        fxStale = true;
        lastParamChangeMs = juce::Time::getMillisecondCounter();
    }
}

void MainComponent::audioProcessorChanged (juce::AudioProcessor* proc, const ChangeDetails& details)
{
    if (details.parameterInfoChanged || details.programChanged)
    {
        if (proc != nullptr && proc == instrumentProc.load())
        {
            instStale = true;
            lastInstChangeMs = juce::Time::getMillisecondCounter();
        }
        else
        {
            fxStale = true;
            lastParamChangeMs = juce::Time::getMillisecondCounter();
        }
    }
}

//==============================================================================
void MainComponent::toggleEditorFor (Graph::Node::Ptr node, const juce::String& name,
                                     std::unique_ptr<juce::DocumentWindow>& window)
{
    if (window != nullptr) { window = nullptr; return; }
    if (node == nullptr)   { setStatus ("No plugin to show."); return; }

    auto* proc = node->getProcessor();
    juce::AudioProcessorEditor* editor = proc->hasEditor() ? proc->createEditorAndMakeActive()
                                                           : new juce::GenericAudioProcessorEditor (*proc);
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (*proc);
    window.reset (new PluginEditorWindow (name, editor));
}

//==============================================================================
void MainComponent::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    midiRecorder.handleMessage (m);   // dashcam capture, independent of the thru switch

    if (! midiThru.load()) return;
    const juce::ScopedLock sl (midiOutLock);
    if (midiOut != nullptr)
        midiOut->sendMessageNow (m);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Device layout may have changed; keep monitor/plugin wiring valid.
    rebuildConnections();
}

void MainComponent::timerCallback()
{
    // --- PRE-RENDER housekeeping ---
    if (preRenderActive())
    {
        if (renderEngine.isRendering())
        {
            lastRenderSpeed = renderEngine.speedX.load();
            const char* fmt = renderEngine.quickMode.load()  ? "Quick update... %d%%  (%.1fx realtime)"
                            : renderEngine.fromCursor.load() ? "Updating from cursor... %d%%  (%.1fx realtime)"
                                                             : "Rendering... %d%%  (%.1fx realtime)";
            renderLabel.setText (juce::String::formatted (fmt,
                                     (int) (renderEngine.progress.load() * 100.0f),
                                     lastRenderSpeed),
                                 juce::dontSendNotification);
        }
        else if (renderEngine.failed.load())
            renderLabel.setText ("Render FAILED", juce::dontSendNotification);
        else if (fxStale.load())
            renderLabel.setText ("FX changed - re-render pending...", juce::dontSendNotification);
        else if (needsFullRefresh)
            renderLabel.setText ("Window fresh - full sweep when knobs settle...", juce::dontSendNotification);
        else if (renderEngine.finished.load())
            renderLabel.setText (juce::String::formatted ("Cache ready  (%.1fx realtime)",
                                                          lastRenderSpeed),
                                 juce::dontSendNotification);

        const auto now = juce::Time::getMillisecondCounter();
        const bool renderIdle = ! renderEngine.isRendering() && ! offlineFxLoading;

        // Debounced quick render after knob movement: only refresh the
        // listening window, so knob-twiddling stays cheap.
        if (fxStale.load() && renderIdle && now - lastParamChangeMs.load() > 600)
        {
            syncOfflineStateAndRender (true);
            needsFullRefresh = true;
        }
        // Janitor: once the knobs have been quiet for a while, do the full
        // cursor-first sweep so the whole cache converges to fresh.
        else if (needsFullRefresh && renderIdle && ! fxStale.load()
                 && now - lastParamChangeMs.load() > 3000)
        {
            needsFullRefresh = false;
            syncOfflineStateAndRender (false);
        }
    }

    // --- GPU FX housekeeping ---
    if (gpuFxButton.getToggleState())
    {
        const auto workerText = gpuWorker.getStatusText();   // empty when ready
        if (workerText.isNotEmpty())
            gpuStatusLabel.setText (workerText, juce::dontSendNotification);
        else if (! gpuDirty.load())
            gpuStatusLabel.setText (gpuReadyText, juce::dontSendNotification);
        else
            gpuStatusLabel.setText ("GPU: knobs changed - re-render pending...", juce::dontSendNotification);

        // Same debounce idea as the FX-knob path: re-render once the GPU
        // knobs have been quiet for 600 ms and the worker is idle.
        const auto now = juce::Time::getMillisecondCounter();
        if (gpuDirty.load() && gpuWorker.isIdleReady()
            && now - lastGpuChangeMs.load() > 600)
        {
            gpuDirty = false;
            requestGpuRender();
        }
    }

    // --- MIDI take recorder housekeeping ---
    {
        const auto saved = midiRecorder.poll();
        if (saved != juce::File())
            setStatus ("MIDI take saved: " + saved.getFileName());

        const auto recText = midiRecorder.isTakeOpen()
            ? juce::String::formatted ("REC * %d", midiRecorder.getNoteCount())
            : juce::String ("MIDI REC");
        if (midiRecButton.getButtonText() != recText)
            midiRecButton.setButtonText (recText);
    }

    // --- MIDI bounce housekeeping ---
    if (bounceEngine.isBouncing())
    {
        midiStatusLabel.setText (juce::String::formatted ("MIDI: bouncing... %d%%  (%.1fx realtime)",
                                     (int) (bounceEngine.progress.load() * 100.0f),
                                     bounceEngine.speedX.load()),
                                 juce::dontSendNotification);
    }
    else if (midiChainActive())
    {
        if (instStale.load())
            midiStatusLabel.setText ("MIDI: VSTi changed - re-bounce pending...",
                                     juce::dontSendNotification);

        // Same debounce as the FX/GPU knobs: re-bounce once the VSTi has been
        // quiet for 600 ms, then hot-swap at the playback position.
        const auto now = juce::Time::getMillisecondCounter();
        if (instStale.load() && ! offlineInstLoading && instrumentNode != nullptr
            && now - lastInstChangeMs.load() > 600)
        {
            instStale = false;
            syncInstStateAndBounce();
        }
    }

    if (! filePlayer->hasFile())
        return;

    auto& t = filePlayer->transport;
    const double pos = t.getCurrentPosition();
    const double len = t.getLengthInSeconds();

    if (! posSlider.isMouseButtonDown())
        posSlider.setValue (pos, juce::dontSendNotification);

    timeLabel.setText (formatTime (pos) + " / " + formatTime (len), juce::dontSendNotification);
    playButton.setButtonText (t.isPlaying() ? "Pause" : "Play");
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff20242b));
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("VST TestBench", 16, 10, getWidth() - 32, 26, juce::Justification::left);
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (34);   // title

    auto row = [&area] (int h) { auto r = area.removeFromTop (h); area.removeFromTop (8); return r; };

    {
        auto r = row (30);
        resetAudioButton.setBounds (r.removeFromRight (110));
        r.removeFromRight (6);
        audioSettingsButton.setBounds (r);
    }

    {
        auto r = row (28);
        sourceLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        sourceCombo.setBounds (r.removeFromLeft (220));
        r.removeFromLeft (8);
        autoBackendButton.setBounds (r);
    }
    {
        auto r = row (28);
        recentLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        recentCombo.setBounds (r.removeFromLeft (r.getWidth() - 140));
        r.removeFromLeft (6);
        loadButton.setBounds (r);
    }
    {
        auto r = row (28);
        inputPairLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        inputPairCombo.setBounds (r.removeFromLeft (120));
    }
    {
        auto r = row (28);
        midiOutLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        midiOutCombo.setBounds (r.removeFromLeft (r.getWidth() - 190));
        r.removeFromLeft (6);
        midiThruButton.setBounds (r);
    }

    // --- instrument stage ---
    {
        auto r = row (30);
        loadInstButton.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        instEditorButton.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (8);
        clearInstButton.setBounds (r.removeFromLeft (110));
    }
    instLabel.setBounds (row (22));

    // --- MIDI bounce + take recorder ---
    {
        auto r = row (28);
        openMidiButton.setBounds (r.removeFromLeft (150));
        r.removeFromLeft (8);
        midiRecButton.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        midiStatusLabel.setBounds (r);
    }

    // --- file player ---
    {
        auto r = row (30);
        openFileButton.setBounds (r.removeFromLeft (150));
        r.removeFromLeft (8);
        playButton.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (8);
        stopButton.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (8);
        loopButton.setBounds (r.removeFromLeft (70));
    }
    {
        auto r = row (26);
        timeLabel.setBounds (r.removeFromRight (110));
        r.removeFromRight (6);
        posSlider.setBounds (r);
    }
    {
        auto r = row (26);
        preRenderButton.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        renderLabel.setBounds (r);
    }
    {
        auto r = row (26);
        gpuFxButton.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        gpuStatusLabel.setBounds (r);
    }
    if (! gpuModules.empty())
    {
        const int contentH = gpuPanelContentHeight();
        auto vp = row (juce::jmin (340, contentH));
        gpuViewport.setBounds (vp);
        const int holderW = vp.getWidth() - (contentH > vp.getHeight() ? 14 : 0);
        gpuPanelHolder.setSize (holderW, contentH);

        int y = 0;
        for (auto& gm : gpuModules)
        {
            gm.enable->setBounds (0, y, holderW - 4, 24);
            y += 28;
            const int n = (int) gm.controls.size();
            const int rows = (n + 1) / 2;
            const int colW = holderW / 2;
            for (int i = 0; i < n; ++i)
            {
                auto cell = juce::Rectangle<int> ((i % 2) * colW, y + (i / 2) * 26,
                                                  colW - 10, 22);
                auto& c = gm.controls[(size_t) i];
                if (c.type == "bool")
                    c.toggle->setBounds (cell);
                else if (c.type == "choice")
                {
                    c.label->setBounds (cell.removeFromLeft (60));
                    c.combo->setBounds (cell);
                }
                else
                {
                    c.label->setBounds (cell.removeFromLeft (118));
                    c.slider->setBounds (cell);
                }
            }
            y += rows * 26 + 6;
        }
    }
    else
        gpuViewport.setBounds ({});
    fileLabel.setBounds (row (22));

    // --- FX stage ---
    {
        auto r = row (30);
        editorButton.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (8);
        bypassButton.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        clearButton.setBounds (r.removeFromLeft (110));
    }

    pluginLabel.setBounds (row (24));
    statusLabel.setBounds (area.removeFromBottom (24));
}
