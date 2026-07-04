#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
// Lightweight VST3 test-bench host.
//   MIDI in  -> (thru)   -> Reface CP (hardware MIDI out)
//   Audio in (UR-RT2 3/4) -> VST      -> Audio out (monitor)
// No folder scanning: plugins are loaded one file at a time and cached.
//==============================================================================
class MainComponent : public juce::Component,
                      private juce::MidiInputCallback,
                      private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using Graph = juce::AudioProcessorGraph;

    //== actions ==
    void showAudioSettings();
    void loadPluginDialog();
    void loadPluginFromDescription (const juce::PluginDescription&);
    void setPluginNode (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void removePlugin();
    void rebuildConnections();
    void refreshMidiOutList();
    void refreshRecentList();
    void openMidiOut (const juce::String& identifier);
    void toggleEditor();
    void saveKnownPlugins();
    void loadKnownPlugins();
    void setStatus (const juce::String&);

    //== callbacks ==
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    //== helpers ==
    juce::File appDir() const;
    juce::File cacheFile() const;
    juce::File audioStateFile() const;
    juce::AudioPluginFormat* vst3Format() const;

    //== audio graph ==
    juce::AudioDeviceManager deviceManager;
    Graph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    Graph::Node::Ptr audioInNode, audioOutNode, midiInNode, pluginNode;
    juce::String currentPluginName;

    //== MIDI thru to Reface ==
    std::unique_ptr<juce::MidiOutput> midiOut;
    juce::CriticalSection            midiOutLock;
    std::atomic<bool>                midiThru { true };

    int inputPairStart = 2;   // 0-based physical channel -> 3/4 by default

    //== UI ==
    juce::TextButton   audioSettingsButton { "Audio / MIDI Settings" };
    juce::TextButton   loadButton          { "Load VST3 file..." };
    juce::TextButton   editorButton        { "Open Plugin UI" };
    juce::TextButton   clearButton         { "Remove Plugin" };
    juce::ToggleButton bypassButton        { "Bypass VST" };
    juce::ToggleButton midiThruButton      { "MIDI thru -> Reface" };
    juce::ComboBox     midiOutCombo, inputPairCombo, recentCombo;
    juce::Label        midiOutLabel  { {}, "Reface MIDI out:" };
    juce::Label        inputPairLabel{ {}, "VST input pair:" };
    juce::Label        recentLabel   { {}, "Recent plugins:" };
    juce::Label        pluginLabel;
    juce::Label        statusLabel;

    std::unique_ptr<juce::DocumentWindow> editorWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
