#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "FilePlayerProcessor.h"
#include "RenderAheadEngine.h"
#include "GpuFxWorker.h"
#include "MidiBounceEngine.h"

//==============================================================================
// Lightweight VST3 test-bench host.
//
//   Source stage (switchable)            FX stage
//   ------------------------------       --------
//   Live input (UR-RT2 pair)      \
//   VST instrument <- MIDI in      >---> VST effect ---> Audio out
//   Audio file player             /
//
//   MIDI in also goes thru to hardware (Reface CP) when enabled.
// No folder scanning: plugins are loaded one file at a time and cached.
//==============================================================================
class MainComponent : public juce::Component,
                      private juce::MidiInputCallback,
                      private juce::ChangeListener,
                      private juce::Timer,
                      private juce::AudioProcessorListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using Graph = juce::AudioProcessorGraph;

    enum SourceMode { srcLive = 1, srcInstrument = 2, srcFile = 3 };

    //== actions ==
    void showAudioSettings();
    void loadPluginDialog (bool asInstrument);
    void loadPluginFromDescription (const juce::PluginDescription&, bool asInstrument);
    void setEffectNode (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void setInstrumentNode (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void removeEffect();
    void removeInstrument();
    void rebuildConnections();
    void refreshMidiOutList();
    void refreshRecentList();
    void openMidiOut (const juce::String& identifier);
    void toggleEditorFor (Graph::Node::Ptr, const juce::String& name,
                          std::unique_ptr<juce::DocumentWindow>& window);
    void saveKnownPlugins();
    void loadKnownPlugins();
    void setStatus (const juce::String&);

    //== file player ==
    void openAudioFileDialog();
    void loadAudioFile (const juce::File&);
    void finishAudioFileLoad (std::unique_ptr<juce::AudioFormatReader>,
                              const juce::File& original, const juce::File& readable);
    void convertWithFfmpegAsync (const juce::File& source);
    juce::String formatTime (double seconds) const;

    //== PRE-RENDER (render-ahead cache) ==
    bool preRenderActive() const;
    void setPreRenderEnabled (bool);
    void beginPreRender();
    void createOfflineFx();
    void syncOfflineStateAndRender (bool quickOnly = false);

    //== MIDI file -> VSTi offline bounce (Phase C) ==
    void openMidiFileDialog();
    void loadMidiFile (const juce::File&);
    void createOfflineInstAndBounce();
    void syncInstStateAndBounce();
    void handleBounceDone (bool ok, juce::File out, juce::String info);
    void abandonMidiChain();
    bool midiChainActive() const;

    //== GPU FX (gpufx worker renders the playable file) ==
    void setGpuFxEnabled (bool);
    void requestGpuRender();
    void handleGpuRenderDone (bool ok, juce::String info, juce::File in, juce::File out);
    void buildGpuPanel (const juce::var& describeResponse);
    juce::var collectGpuParams() const;
    juce::File effectivePlayableFile() const;
    bool swapPlayableFilePreservingPosition (const juce::File&);

    //== callbacks ==
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;

    //== audio backend per source mode (WASAPI for file playback, ASIO for live) ==
    void applyBackendForMode (int mode);
    void saveBackendSnapshot();
    juce::File backendStateFile (const juce::String& typeName) const;

    //== helpers ==
    juce::File appDir() const;
    juce::File cacheFile() const;
    juce::File audioStateFile() const;
    juce::File lastDirFile() const;
    juce::File lastMidiDirFile() const;
    juce::File autoBackendFile() const;
    juce::AudioPluginFormat* vst3Format() const;
    int currentSourceMode() const;

    //== audio graph ==
    juce::AudioDeviceManager deviceManager;
    Graph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioFormatManager audioFormats;

    Graph::Node::Ptr audioInNode, audioOutNode, midiInNode;
    Graph::Node::Ptr effectNode, instrumentNode, filePlayerNode;
    FilePlayerProcessor* filePlayer = nullptr;   // owned by the graph node
    juce::String currentEffectName, currentInstrumentName;
    juce::PluginDescription currentInstDesc;

    //== PRE-RENDER state ==
    RenderCache renderCache;
    std::unique_ptr<CacheAudioSource> cacheSource;
    RenderAheadEngine renderEngine;
    std::unique_ptr<juce::AudioPluginInstance> offlineFx;   // clone of the live FX, render thread only
    bool offlineFxLoading = false;
    juce::PluginDescription currentFxDesc;
    juce::File currentPlayableFile, currentOriginalFile;    // playable = decoded (ffmpeg) file
    juce::int64 fileLengthSamples = 0;
    double fileSampleRate = 0.0;
    std::atomic<bool> fxStale { false };
    std::atomic<juce::uint32> lastParamChangeMs { 0 };
    float lastRenderSpeed = 0.0f;
    bool  needsFullRefresh = false;   // quick renders leave the rest of the file stale

    //== MIDI bounce state (Phase C) ==
    MidiBounceEngine bounceEngine;
    std::unique_ptr<juce::AudioPluginInstance> offlineInst;  // clone of the live VSTi, bounce thread only
    bool offlineInstLoading = false;
    juce::MidiMessageSequence midiSequence;   // merged tracks, timestamps in seconds
    juce::File currentMidiFile;
    int  bounceGeneration = 0;
    bool firstBouncePending = false;          // bounce result should (re)load the file player
    std::atomic<bool> instStale { false };
    std::atomic<juce::uint32> lastInstChangeMs { 0 };
    std::atomic<juce::AudioProcessor*> instrumentProc { nullptr };  // listener-thread-safe identity
    juce::String midiReadyText { "MIDI: none" };

    //== GPU FX state ==
    GpuFxWorker gpuWorker;
    juce::File  currentGpuFile;       // last completed gpufx output (generation file)
    int         gpuGeneration = 0;
    std::atomic<bool> gpuDirty { false };
    std::atomic<juce::uint32> lastGpuChangeMs { 0 };
    juce::String gpuReadyText { "GPU: worker ready" };

    struct GpuControl
    {
        juce::String name;
        bool isBool = false;
        std::unique_ptr<juce::Label>        label;
        std::unique_ptr<juce::Slider>       slider;
        std::unique_ptr<juce::ToggleButton> toggle;
    };
    std::vector<GpuControl> gpuControls;   // built from the worker's describe schema

    //== MIDI thru to Reface ==
    std::unique_ptr<juce::MidiOutput> midiOut;
    juce::CriticalSection            midiOutLock;
    std::atomic<bool>                midiThru { true };

    int inputPairStart = 2;   // 0-based physical channel -> 3/4 by default

    //== UI ==
    juce::TextButton   audioSettingsButton { "Audio / MIDI Settings" };
    juce::TextButton   resetAudioButton    { "Reset ASIO" };
    juce::ComboBox     sourceCombo;
    juce::Label        sourceLabel { {}, "Source:" };
    juce::ToggleButton autoBackendButton { "Auto backend (file=WASAPI / live=ASIO)" };

    // FX stage
    juce::TextButton   loadButton   { "Load FX VST3..." };
    juce::TextButton   editorButton { "FX UI" };
    juce::TextButton   clearButton  { "Remove FX" };
    juce::ToggleButton bypassButton { "Bypass FX" };

    // Instrument stage
    juce::TextButton   loadInstButton  { "Load VSTi..." };
    juce::TextButton   instEditorButton{ "Inst UI" };
    juce::TextButton   clearInstButton { "Remove Inst" };
    juce::Label        instLabel;

    // MIDI bounce (Phase C)
    juce::TextButton   openMidiButton { "Open MIDI file..." };
    juce::Label        midiStatusLabel;

    // File player
    juce::TextButton   openFileButton { "Open audio file..." };
    juce::TextButton   playButton     { "Play" };
    juce::TextButton   stopButton     { "Stop" };
    juce::ToggleButton loopButton     { "Loop" };
    juce::Slider       posSlider;
    juce::Label        timeLabel;
    juce::Label        fileLabel;
    juce::ToggleButton preRenderButton { "PRE-RENDER" };
    juce::Label        renderLabel;
    juce::ToggleButton gpuFxButton { "GPU FX" };
    juce::Label        gpuStatusLabel;

    juce::ToggleButton midiThruButton { "MIDI thru -> Reface" };
    juce::ComboBox     midiOutCombo, inputPairCombo, recentCombo;
    juce::Label        midiOutLabel  { {}, "Reface MIDI out:" };
    juce::Label        inputPairLabel{ {}, "Live input pair:" };
    juce::Label        recentLabel   { {}, "Cached plugins:" };
    juce::Label        pluginLabel;
    juce::Label        statusLabel;

    std::unique_ptr<juce::DocumentWindow> editorWindow, instEditorWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
