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
    juce::addDefaultFormatsToManager (formatManager);   // AudioUnit (macOS) + VST3
    loadKnownPlugins();

    // --- audio + MIDI device ---
    std::unique_ptr<juce::XmlElement> savedAudio (juce::XmlDocument::parse (audioStateFile()));
    deviceManager.initialise (8, 2, savedAudio.get(), true);   // up to 8 inputs / 2 outputs
    deviceManager.addChangeListener (this);

    // --- processor graph: fixed IO nodes ---
    audioInNode  = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioInputNode));
    audioOutNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode));
    midiInNode   = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::midiInputNode));

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    deviceManager.addMidiInputDeviceCallback ({}, &player);   // MIDI into the graph (instrument)
    deviceManager.addMidiInputDeviceCallback ({}, this);      // our own thru-to-Reface path
    enableAllMidiInputs();   // catch-all callbacks only fire for ENABLED devices
    rebuildConnections();

    // === UI ===
    auto add = [this] (juce::Component& c) { addAndMakeVisible (c); };
    add (audioSettingsButton); add (scanButton);
    add (instrumentCombo); add (instFileButton); add (instEditorButton); add (instClearButton);
    add (effectCombo);     add (fxFileButton);   add (fxEditorButton);   add (fxClearButton);
    add (bypassButton);    add (midiThruButton); add (useInputButton);
    add (midiOutCombo);    add (inputPairCombo);

    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    scanButton.onClick          = [this] { refreshPluginList(); };

    add (searchLabel); add (searchBox);
    searchLabel.setJustificationType (juce::Justification::centredRight);
    searchBox.setTextToShowWhenEmpty ("type to filter plugins...", juce::Colours::grey);
    searchBox.onTextChange = [this] { populateCombos(); };

    instFileButton.onClick   = [this] { loadFileIntoSlot (true); };
    instEditorButton.onClick = [this] { toggleEditor (true); };
    instClearButton.onClick  = [this] { removeSlot (true); };
    fxFileButton.onClick     = [this] { loadFileIntoSlot (false); };
    fxEditorButton.onClick   = [this] { toggleEditor (false); };
    fxClearButton.onClick    = [this] { removeSlot (false); };

    midiThruButton.setToggleState (true, juce::dontSendNotification);
    midiThruButton.onClick = [this] { midiThru = midiThruButton.getToggleState(); };

    bypassButton.onClick = [this]
    {
        if (effect.node != nullptr)
            effect.node->setBypassed (bypassButton.getToggleState());
    };

    useInputButton.onClick = [this]
    {
        useAudioInput = useInputButton.getToggleState();
        rebuildConnections();
    };

    instrumentCombo.setTextWhenNothingSelected ("(refresh list or load a file)");
    effectCombo.setTextWhenNothingSelected     ("(refresh list or load a file)");
    instrumentCombo.onChange = [this]
    {
        const int id = instrumentCombo.getSelectedId();
        if (id <= 1) { removeSlot (true); return; }
        scanSelected (id - 2, true);
    };
    effectCombo.onChange = [this]
    {
        const int id = effectCombo.getSelectedId();
        if (id <= 1) { removeSlot (false); return; }
        scanSelected (id - 2, false);
    };

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

    for (auto* l : { &instrumentLabel, &effectLabel, &midiOutLabel, &inputPairLabel })
    {
        l->setJustificationType (juce::Justification::centredRight);
        add (*l);
    }

    chainLabel.setColour (juce::Label::textColourId, juce::Colours::aqua);
    add (chainLabel);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    add (statusLabel);

    refreshMidiOutList();
    updateChainLabel();
    setStatus ("Ready. Load an Instrument (MIDI source) + an Effect, then play the Reface CP.");
    refreshPluginList();   // cheap enumeration (names only, no instantiation)

    setSize (720, 480);
}

MainComponent::~MainComponent()
{
    instrument.editor = nullptr;
    effect.editor     = nullptr;

    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeMidiInputDeviceCallback ({}, &player);
    deviceManager.removeAudioCallback (&player);
    deviceManager.removeChangeListener (this);
    player.setProcessor (nullptr);

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

void MainComponent::populateCombos()
{
    const int keepInst = instrumentCombo.getSelectedId();
    const int keepFx   = effectCombo.getSelectedId();
    const auto filter  = searchBox.getText().trim();

    instrumentCombo.clear (juce::dontSendNotification);
    effectCombo.clear (juce::dontSendNotification);
    instrumentCombo.addItem ("(none)", 1);
    effectCombo.addItem ("(none)", 1);

    // Instrument picker shows instruments (+ still-unclassified plugins);
    // effect picker shows effects (+ still-unclassified). Item IDs stay tied to
    // the master candidates index (id = index + 2) regardless of filtering.
    for (int i = 0; i < candidates.size(); ++i)
    {
        const auto& c = candidates.getReference (i);
        if (filter.isNotEmpty() && ! c.name.containsIgnoreCase (filter))
            continue;

        const auto label = c.name + "  [" + c.format->getName() + "]";
        if (c.kind == kInstrument || c.kind == kUnknown) instrumentCombo.addItem (label, i + 2);
        if (c.kind == kEffect     || c.kind == kUnknown) effectCombo.addItem     (label, i + 2);
    }

    instrumentCombo.setSelectedId (keepInst > 0 ? keepInst : 0, juce::dontSendNotification);
    effectCombo.setSelectedId     (keepFx   > 0 ? keepFx   : 0, juce::dontSendNotification);
}

//==============================================================================
// Determine instrument vs effect WITHOUT instantiating the plugin:
//  1) if we already scanned it once, trust the cached PluginDescription;
//  2) else, for AudioUnits, parse the type code embedded in the identifier
//     ("AudioUnit:Category/TYPE,subtype,manu" -> 'aumu' == instrument);
//  3) else unknown (uncached VST3) -> shown in both lists until first loaded.
int MainComponent::classifyCandidate (juce::AudioPluginFormat* fmt, const juce::String& id) const
{
    for (const auto& t : knownPlugins.getTypes())
        if (t.fileOrIdentifier == id)
            return t.isInstrument ? kInstrument : kEffect;

    if (fmt->getName().containsIgnoreCase ("AudioUnit"))
    {
        const int slash = id.indexOfChar ('/');
        if (slash >= 0)
        {
            const auto type = id.substring (slash + 1, slash + 5);
            return type == "aumu" ? kInstrument : kEffect;
        }
    }

    return kUnknown;
}

//==============================================================================
// Cheap discovery: list plugin names/identifiers WITHOUT instantiating any of
// them, so one crashing plugin can never take the whole scan (and the app) down.
void MainComponent::refreshPluginList()
{
    candidates.clearQuick();

    for (auto* fmt : formatManager.getFormats())
    {
        const auto ids = fmt->searchPathsForPlugins (fmt->getDefaultLocationsToSearch(), true, false);
        for (const auto& id : ids)
        {
            Candidate c;
            c.format = fmt;
            c.id     = id;
            c.name   = fmt->getNameOfPluginFromIdentifier (id);
            if (c.name.isEmpty())
                c.name = juce::File::createFileWithoutCheckingPath (id).getFileNameWithoutExtension();
            c.kind   = classifyCandidate (fmt, id);
            candidates.add (c);
        }
    }

    populateCombos();
    setStatus ("Found " + juce::String (candidates.size())
               + " plugins. Pick one per slot (only the selected plugin is loaded).");
}

//==============================================================================
void MainComponent::enableAllMidiInputs()
{
    for (const auto& d : juce::MidiInput::getAvailableDevices())
        if (! deviceManager.isMidiInputDeviceEnabled (d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
}

//==============================================================================
// Instantiate ONLY the selected candidate, then place it in the chosen slot.
void MainComponent::scanSelected (int candidateIndex, bool asInstrument)
{
    if (candidateIndex < 0 || candidateIndex >= candidates.size())
        return;

    const auto c = candidates.getReference (candidateIndex);

    juce::OwnedArray<juce::PluginDescription> found;
    setStatus ("Scanning " + c.name + " ...");
    knownPlugins.scanAndAddFile (c.id, true, found, *c.format);
    saveKnownPlugins();

    if (found.isEmpty())
    {
        setStatus ("Could not read " + c.name + " (skipped).");
        return;
    }

    // Now that it is scanned, record its true type so the pickers classify it
    // correctly from here on.
    candidates.getReference (candidateIndex).kind = found.getFirst()->isInstrument ? kInstrument : kEffect;

    loadIntoSlot (*found.getFirst(), asInstrument);
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        deviceManager,
        0, 8,      // min / max input channels
        0, 2,      // min / max output channels
        true,      // show MIDI inputs
        false,     // show MIDI outputs (Reface handled via our own combo)
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
void MainComponent::loadFileIntoSlot (bool asInstrument)
{
    auto vst3Dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Audio/Plug-Ins/VST3");

    auto chooser = std::make_shared<juce::FileChooser> (
        asInstrument ? "Select a VST3 instrument" : "Select a VST3 effect",
        vst3Dir.isDirectory() ? vst3Dir : juce::File(),
        "*.vst3");

    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* f : formatManager.getFormats())
        if (f->getName().containsIgnoreCase ("VST3"))
            vst3 = f;

    if (vst3 == nullptr) { setStatus ("VST3 format not available."); return; }

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectDirectories,
        [this, chooser, vst3, asInstrument] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;

            juce::OwnedArray<juce::PluginDescription> found;
            setStatus ("Scanning " + file.getFileName() + " ...");
            knownPlugins.scanAndAddFile (file.getFullPathName(), true, found, *vst3);
            saveKnownPlugins();

            if (found.isEmpty())
                setStatus ("No plugin found in " + file.getFileName());
            else
                loadIntoSlot (*found.getFirst(), asInstrument);
        });
}

void MainComponent::loadIntoSlot (const juce::PluginDescription& desc, bool asInstrument)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    const double rate  = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    const int    block = setup.bufferSize  > 0 ? setup.bufferSize  : 512;

    setStatus ("Loading " + desc.name + " ...");
    formatManager.createPluginInstanceAsync (
        desc, rate, block,
        [this, desc, asInstrument] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                    const juce::String& error)
        {
            if (instance == nullptr) { setStatus ("Load failed: " + error); return; }
            setSlot (std::move (instance), desc, asInstrument);
        });
}

void MainComponent::setSlot (std::unique_ptr<juce::AudioPluginInstance> instance,
                             const juce::PluginDescription& desc, bool asInstrument)
{
    auto& slot = asInstrument ? instrument : effect;

    slot.editor = nullptr;
    if (slot.node != nullptr)
        graph.removeNode (slot.node->nodeID);

    instance->enableAllBuses();

    auto setup = deviceManager.getAudioDeviceSetup();
    instance->setPlayConfigDetails (instance->getTotalNumInputChannels(),
                                    instance->getTotalNumOutputChannels(),
                                    setup.sampleRate > 0 ? setup.sampleRate : 48000.0,
                                    setup.bufferSize  > 0 ? setup.bufferSize  : 512);

    slot.node = graph.addNode (std::move (instance));
    slot.name = desc.name;

    if (! asInstrument)
        slot.node->setBypassed (bypassButton.getToggleState());

    rebuildConnections();
    updateChainLabel();
    setStatus ("Loaded " + juce::String (asInstrument ? "instrument: " : "effect: ") + desc.name);
}

void MainComponent::removeSlot (bool asInstrument)
{
    auto& slot = asInstrument ? instrument : effect;
    slot.editor = nullptr;
    if (slot.node != nullptr)
    {
        graph.removeNode (slot.node->nodeID);
        slot.node = nullptr;
    }
    slot.name = {};
    rebuildConnections();
    updateChainLabel();
    setStatus (juce::String (asInstrument ? "Instrument" : "Effect") + " removed.");
}

//==============================================================================
void MainComponent::rebuildConnections()
{
    for (auto c : graph.getConnections())
        graph.removeConnection (c);

    const int outChans = 2;

    // MIDI -> instrument and effect (harmless where the plugin ignores MIDI).
    if (instrument.node != nullptr)
        graph.addConnection ({ { midiInNode->nodeID, Graph::midiChannelIndex },
                               { instrument.node->nodeID, Graph::midiChannelIndex } });
    if (effect.node != nullptr)
        graph.addConnection ({ { midiInNode->nodeID, Graph::midiChannelIndex },
                               { effect.node->nodeID, Graph::midiChannelIndex } });

    // Clamp the audio-input pair to the channels the device actually exposes,
    // so we never create an invalid (silently dropped) connection.
    const int ainChans = audioInNode->getProcessor()->getTotalNumOutputChannels();
    int pair = inputPairStart;
    if (pair + 2 > ainChans)
        pair = juce::jmax (0, ainChans - 2);

    // Feed the source bus (instrument out + optional audio input, summed) into dst.
    auto feedInto = [&] (Graph::Node::Ptr dst, int dstInChans)
    {
        const int n = juce::jmin (2, dstInChans);

        if (instrument.node != nullptr)
        {
            const int io = juce::jmax (1, instrument.node->getProcessor()->getTotalNumOutputChannels());
            for (int ch = 0; ch < n; ++ch)
                graph.addConnection ({ { instrument.node->nodeID, juce::jmin (ch, io - 1) },
                                       { dst->nodeID, ch } });
        }

        if (useAudioInput && ainChans >= 2)
        {
            for (int ch = 0; ch < n; ++ch)
                graph.addConnection ({ { audioInNode->nodeID, pair + ch },
                                       { dst->nodeID, ch } });
        }
    };

    if (effect.node != nullptr)
    {
        const int ein  = juce::jmax (2, effect.node->getProcessor()->getTotalNumInputChannels());
        feedInto (effect.node, ein);

        const int eout = effect.node->getProcessor()->getTotalNumOutputChannels();
        for (int ch = 0; ch < juce::jmin (outChans, eout); ++ch)
            graph.addConnection ({ { effect.node->nodeID, ch },
                                   { audioOutNode->nodeID, ch } });
    }
    else
    {
        // No effect: monitor the source (instrument and/or audio input) directly.
        feedInto (audioOutNode, outChans);
    }
}

void MainComponent::updateChainLabel()
{
    juce::String chain = "MIDI";
    chain << "  ->  " << (instrument.name.isNotEmpty() ? instrument.name : juce::String ("(no instrument)"));
    if (useAudioInput) chain << "  (+audio in)";
    chain << "  ->  " << (effect.name.isNotEmpty() ? effect.name : juce::String ("(no effect / monitor)"));
    chain << "  ->  out";
    chainLabel.setText (chain, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::toggleEditor (bool asInstrument)
{
    auto& slot = asInstrument ? instrument : effect;

    if (slot.editor != nullptr) { slot.editor = nullptr; return; }
    if (slot.node == nullptr)   { setStatus ("Nothing loaded in that slot."); return; }

    auto* proc = slot.node->getProcessor();
    juce::AudioProcessorEditor* editor = proc->hasEditor() ? proc->createEditorAndMakeActive()
                                                           : new juce::GenericAudioProcessorEditor (*proc);
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (*proc);
    slot.editor.reset (new PluginEditorWindow (slot.name, editor));
}

//==============================================================================
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
void MainComponent::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    if (! midiThru.load()) return;
    const juce::ScopedLock sl (midiOutLock);
    if (midiOut != nullptr)
        midiOut->sendMessageNow (m);
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Device layout may have changed; keep wiring valid and MIDI flowing.
    enableAllMidiInputs();
    rebuildConnections();
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
        audioSettingsButton.setBounds (r.removeFromLeft (200));
        r.removeFromLeft (8);
        scanButton.setBounds (r.removeFromLeft (200));
    }
    {
        auto r = row (26);
        searchLabel.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (6);
        searchBox.setBounds (r);
    }
    {
        auto r = row (28);
        instrumentLabel.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (6);
        instClearButton.setBounds  (r.removeFromRight (80));
        instEditorButton.setBounds (r.removeFromRight (46));
        instFileButton.setBounds   (r.removeFromRight (64));
        instrumentCombo.setBounds  (r);
    }
    {
        auto r = row (28);
        effectLabel.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (6);
        fxClearButton.setBounds  (r.removeFromRight (80));
        fxEditorButton.setBounds (r.removeFromRight (46));
        fxFileButton.setBounds   (r.removeFromRight (64));
        effectCombo.setBounds    (r);
    }
    {
        auto r = row (28);
        useInputButton.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (12);
        inputPairLabel.setBounds (r.removeFromLeft (100));
        r.removeFromLeft (6);
        inputPairCombo.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (12);
        bypassButton.setBounds (r.removeFromLeft (110));
    }
    {
        auto r = row (28);
        midiOutLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        midiOutCombo.setBounds (r.removeFromLeft (r.getWidth() - 190));
        r.removeFromLeft (6);
        midiThruButton.setBounds (r);
    }

    chainLabel.setBounds (row (26));
    statusLabel.setBounds (area.removeFromBottom (24));
}
