#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>

//==============================================================================
// VST/AU test-bench host with a fixed Instrument -> Effect chain.
//
//   MIDI in (reface CP) --> [Instrument slot] --\
//                                                >-- [Effect slot] --> Audio out
//   Audio in (optional, ch-selectable) ---------/
//
// Rationale: driving an INSTRUMENT from MIDI gives a reliable internal signal
// source, so testing an effect no longer depends on live audio-input channel
// routing (which is easy to mis-map on multi-in interfaces).
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

    // One loadable plugin position in the chain.
    struct Slot
    {
        Graph::Node::Ptr                       node;
        juce::String                           name;
        std::unique_ptr<juce::DocumentWindow>  editor;
    };

    enum Kind { kUnknown = 0, kInstrument = 1, kEffect = 2 };

    // A discovered-but-not-yet-instantiated plugin (cheap to enumerate).
    struct Candidate
    {
        juce::AudioPluginFormat* format = nullptr;
        juce::String             id;      // file path (VST3) or identifier (AU)
        juce::String             name;
        int                      kind = kUnknown;
    };

    //== actions ==
    void showAudioSettings();
    void refreshPluginList();
    int  classifyCandidate (juce::AudioPluginFormat*, const juce::String& id) const;
    void enableAllMidiInputs();
    void scanSelected (int candidateIndex, bool asInstrument);
    void populateCombos();
    void loadFileIntoSlot (bool asInstrument);
    void loadIntoSlot (const juce::PluginDescription&, bool asInstrument);
    void setSlot (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&, bool asInstrument);
    void removeSlot (bool asInstrument);
    void toggleEditor (bool asInstrument);
    void rebuildConnections();
    void updateChainLabel();
    void refreshMidiOutList();
    void openMidiOut (const juce::String& identifier);
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

    //== audio graph ==
    juce::AudioDeviceManager        deviceManager;
    Graph                           graph;
    juce::AudioProcessorPlayer      player;
    juce::AudioPluginFormatManager  formatManager;
    juce::KnownPluginList           knownPlugins;

    Graph::Node::Ptr audioInNode, audioOutNode, midiInNode;
    Slot instrument, effect;

    juce::Array<Candidate> candidates;   // discovered plugins for the pickers

    //== MIDI thru to Reface ==
    std::unique_ptr<juce::MidiOutput> midiOut;
    juce::CriticalSection             midiOutLock;
    std::atomic<bool>                 midiThru { true };

    //== audio-input source (optional) ==
    int  inputPairStart = 2;      // 0-based physical channel -> 3/4 by default
    bool useAudioInput  = false;  // instrument is the primary source; input is opt-in

    //== UI ==
    juce::TextButton   audioSettingsButton { "Audio / MIDI Settings" };
    juce::TextButton   scanButton          { "Refresh plugin list (AU + VST3)" };

    juce::TextEditor   searchBox;
    juce::Label        searchLabel { {}, "Filter:" };

    juce::ComboBox     instrumentCombo, effectCombo, midiOutCombo, inputPairCombo;
    juce::TextButton   instFileButton { "File..." };
    juce::TextButton   instEditorButton { "UI" };
    juce::TextButton   instClearButton  { "Remove" };
    juce::TextButton   fxFileButton   { "File..." };
    juce::TextButton   fxEditorButton { "UI" };
    juce::TextButton   fxClearButton  { "Remove" };
    juce::ToggleButton bypassButton   { "Bypass FX" };
    juce::ToggleButton midiThruButton { "MIDI thru -> Reface" };
    juce::ToggleButton useInputButton { "Use audio input" };

    juce::Label        instrumentLabel { {}, "Instrument:" };
    juce::Label        effectLabel     { {}, "Effect:" };
    juce::Label        midiOutLabel    { {}, "Reface MIDI out:" };
    juce::Label        inputPairLabel  { {}, "Audio-in pair:" };
    juce::Label        chainLabel;
    juce::Label        statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
