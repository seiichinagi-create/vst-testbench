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
    loadKnownPlugins();

    // --- audio + MIDI device ---
    std::unique_ptr<juce::XmlElement> savedAudio (juce::XmlDocument::parse (audioStateFile()));
    deviceManager.initialise (8, 2, savedAudio.get(), true);   // request up to 8 inputs / 2 outputs
    deviceManager.addChangeListener (this);

    // --- processor graph: build the fixed IO nodes ---
    audioInNode  = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioInputNode));
    audioOutNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode));
    midiInNode   = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::midiInputNode));

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);
    deviceManager.addMidiInputDeviceCallback ({}, &player);   // feed MIDI into graph (for VSTi)
    deviceManager.addMidiInputDeviceCallback ({}, this);      // our own thru-to-Reface path
    rebuildConnections();

    // === UI ===
    auto addBtn = [this] (juce::Button& b) { addAndMakeVisible (b); };
    addBtn (audioSettingsButton); addBtn (loadButton); addBtn (editorButton);
    addBtn (clearButton);         addBtn (bypassButton); addBtn (midiThruButton);

    audioSettingsButton.onClick = [this] { showAudioSettings(); };
    loadButton.onClick          = [this] { loadPluginDialog(); };
    editorButton.onClick        = [this] { toggleEditor(); };
    clearButton.onClick         = [this] { removePlugin(); };

    midiThruButton.setToggleState (true, juce::dontSendNotification);
    midiThruButton.onClick = [this] { midiThru = midiThruButton.getToggleState(); };
    bypassButton.onClick   = [this]
    {
        if (pluginNode != nullptr)
            pluginNode->setBypassed (bypassButton.getToggleState());
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
            loadPluginFromDescription (types[idx]);
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
    pluginLabel.setText ("No plugin loaded (input -> output monitor)", juce::dontSendNotification);

    setSize (640, 380);
}

MainComponent::~MainComponent()
{
    editorWindow = nullptr;
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

juce::AudioPluginFormat* MainComponent::vst3Format() const
{
    for (auto* f : formatManager.getFormats())
        if (f->getName().containsIgnoreCase ("VST3"))
            return f;
    return nullptr;
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
        recentCombo.addItem (types[i].name, i + 1);
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
void MainComponent::loadPluginDialog()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Select a VST3 plugin",
        juce::File ("C:/Program Files/Common Files/VST3"),
        "*.vst3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectDirectories,
        [this, chooser] (const juce::FileChooser& fc)
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
                loadPluginFromDescription (*found.getFirst());
        });
}

void MainComponent::loadPluginFromDescription (const juce::PluginDescription& desc)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    const double rate  = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    const int    block = setup.bufferSize  > 0 ? setup.bufferSize  : 512;

    setStatus ("Loading " + desc.name + " ...");
    formatManager.createPluginInstanceAsync (
        desc, rate, block,
        [this, desc] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            if (instance == nullptr)
            {
                setStatus ("Load failed: " + error);
                return;
            }
            setPluginNode (std::move (instance), desc);
        });
}

void MainComponent::setPluginNode (std::unique_ptr<juce::AudioPluginInstance> instance,
                                   const juce::PluginDescription& desc)
{
    editorWindow = nullptr;
    if (pluginNode != nullptr)
        graph.removeNode (pluginNode->nodeID);

    instance->enableAllBuses();

    auto setup = deviceManager.getAudioDeviceSetup();
    instance->setPlayConfigDetails (instance->getTotalNumInputChannels(),
                                    instance->getTotalNumOutputChannels(),
                                    setup.sampleRate > 0 ? setup.sampleRate : 48000.0,
                                    setup.bufferSize  > 0 ? setup.bufferSize  : 512);

    pluginNode = graph.addNode (std::move (instance));
    pluginNode->setBypassed (bypassButton.getToggleState());
    currentPluginName = desc.name;

    rebuildConnections();
    pluginLabel.setText ("VST: " + desc.name + "  ("
                         + juce::String (pluginNode->getProcessor()->getTotalNumInputChannels()) + " in / "
                         + juce::String (pluginNode->getProcessor()->getTotalNumOutputChannels()) + " out)",
                         juce::dontSendNotification);
    setStatus ("Loaded " + desc.name);
}

void MainComponent::removePlugin()
{
    editorWindow = nullptr;
    if (pluginNode != nullptr)
    {
        graph.removeNode (pluginNode->nodeID);
        pluginNode = nullptr;
    }
    currentPluginName = {};
    rebuildConnections();
    pluginLabel.setText ("No plugin loaded (input -> output monitor)", juce::dontSendNotification);
    setStatus ("Plugin removed.");
}

//==============================================================================
void MainComponent::rebuildConnections()
{
    for (auto c : graph.getConnections())
        graph.removeConnection (c);

    const int outChans = 2;

    if (pluginNode != nullptr)
    {
        auto* proc = pluginNode->getProcessor();
        const int pin  = juce::jmax (0, proc->getTotalNumInputChannels());
        const int pout = juce::jmax (0, proc->getTotalNumOutputChannels());

        // audio in (chosen pair) -> plugin inputs
        for (int ch = 0; ch < juce::jmin (2, pin); ++ch)
            graph.addConnection ({ { audioInNode->nodeID,  inputPairStart + ch },
                                   { pluginNode->nodeID,   ch } });

        // plugin outputs -> device out
        for (int ch = 0; ch < juce::jmin (outChans, pout); ++ch)
            graph.addConnection ({ { pluginNode->nodeID,  ch },
                                   { audioOutNode->nodeID, ch } });

        // MIDI in -> plugin (for VSTi testing)
        graph.addConnection ({ { midiInNode->nodeID,  Graph::midiChannelIndex },
                               { pluginNode->nodeID,  Graph::midiChannelIndex } });
    }
    else
    {
        // No plugin: monitor the chosen input pair straight to the outputs.
        for (int ch = 0; ch < outChans; ++ch)
            graph.addConnection ({ { audioInNode->nodeID,  inputPairStart + ch },
                                   { audioOutNode->nodeID, ch } });
    }
}

//==============================================================================
void MainComponent::toggleEditor()
{
    if (editorWindow != nullptr) { editorWindow = nullptr; return; }
    if (pluginNode == nullptr)   { setStatus ("No plugin to show."); return; }

    auto* proc = pluginNode->getProcessor();
    juce::AudioProcessorEditor* editor = proc->hasEditor() ? proc->createEditorAndMakeActive()
                                                           : new juce::GenericAudioProcessorEditor (*proc);
    if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor (*proc);
    editorWindow.reset (new PluginEditorWindow (currentPluginName, editor));
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
    // Device layout may have changed; keep monitor/plugin wiring valid.
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

    audioSettingsButton.setBounds (row (30));

    {
        auto r = row (28);
        recentLabel.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (6);
        recentCombo.setBounds (r.removeFromLeft (r.getWidth() - 130));
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
    {
        auto r = row (30);
        editorButton.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (8);
        bypassButton.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        clearButton.setBounds (r.removeFromLeft (130));
    }

    pluginLabel.setBounds (row (24));
    statusLabel.setBounds (area.removeFromBottom (24));
}
