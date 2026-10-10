#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "vendor/SnLookAndFeel.h"
#include "FilePlayerProcessor.h"
#include "RenderAheadEngine.h"
#include "GpuFxWorker.h"
#include "MidiBounceEngine.h"
#include "MidiTakeRecorder.h"
#include "MpeSupport.h"
#include "OutputTap.h"
#include "MixStrips.h"
#include "MixerPanel.h"
#include "RigWorkerClient.h"
#include "ControlServer.h"

//==============================================================================
// Lightweight VST3 test-bench host.
//
//   Source stage (switchable)            FX stage
//   ------------------------------       --------
//   Live input (UR-RT2 pair)      \
//   VST instrument <- MIDI in      >---> VST effect ---> Audio out
//   Audio file player             /
//
//   MIDI in can also go thru to a hardware synth (MIDI out combo) when enabled.
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
    void loadPluginDialog (bool asInstrument, int track = 1);
    void loadPluginFromDescription (const juce::PluginDescription&, bool asInstrument, int track = 1);
    void setEffectNode (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void setInstrumentNode (std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void removeEffect();
    void removeInstrument();
    // INST 2 and INST 3 (track = trkInst2 / trkInst3): live MIDI only for now (the MIDI-file bounce, MPE setup and
    // PRE-RENDER stay with INST 1; docs/TRACKS.md P4)
    void setExtraInstrument (int track, std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void removeExtraInstrument (int track);
    static int trackFromRole (const juce::String& role);   // "inst" -> 1, "inst2" -> 2, "inst3" -> 3, else -1
    static juce::uint32 maskFromChannels (const juce::var&);
    static int trackIndexOf (const juce::String&);
    juce::var trackInfo (int track) const;                 // -1 = master
    void rebuildConnections();
    // The legacy source mode as a preset for the strips: live / file -> AUDIO sounds; VSTi -> the instruments sound
    // (AUDIO again when none is loaded). Applied when the mode or what is loaded changes, not on every rebuild: the
    // mute buttons are the user's in between.
    void applyModePreset();
    void updateSolo();       // silence the tracks that are not soloed while any track is
    // A strip, a MIDI channel mask or a plug-in of a track changed: the pre-render cache (AUDIO strip, master) and the MIDI
    // bounce (INST tracks) are stale. track: 0 AUDIO, 1..3 INST, 4 / -1 master.
    void markMixChanged (int track);
    void markBakeDirty();              // the PRE-RENDER mix bake is out of date (the timer re-bakes once things are quiet)

    //== PRE-RENDER by the rig (docs/TRACKS.md): with inserts or send buses loaded the cache is a whole-mix render by the rig worker ==
    bool mixBakeMode = false;          // PRE-RENDER is on and the rig bakes it (else the in-process engine does: file + master FX)
    bool mixBakeReady = false;         // a bake has landed and the cache plays
    bool mixBakeRunning = false;
    std::atomic<bool> mixBakeDirty { false };
    std::atomic<juce::uint32> mixBakeLastChangeMs { 0 };
    RenderCache bakeCache[2];          // the playing one and the one being filled (swapped when a bake lands)
    std::unique_ptr<CacheAudioSource> bakeSource[2];
    int bakeSlotInUse = 0, bakeGeneration = 0;
    juce::File currentBakeFile;
    void startMixBake();
    void finishMixBake (bool ok, const juce::File& out, const juce::String& info);
    bool cacheInPath() const { return preRenderActive() && (! mixBakeMode || mixBakeReady); }   // the cache is what plays
    // What the mixer shows under a strip name (insert = false) or in its insert slot (insert = true).
    // index: 0 AUDIO, 1..3 INST, 4..5 SEND 1-2, 6 MASTER (the mixer panel's numbering).
    juce::String describeTrack (int index, bool insert = false) const;
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

    //== MPE (live injection + MIDI-file bounce) ==
    void injectMidi (const juce::MidiMessage&);     // any thread: graph + take recorder + thru
    void sendMpeSetupLive();                        // zone / bend-range setup to the live VSTi (and thru)
    void setMpeEnabled (bool, bool rebounce = true);
    // merged file sequence -> what the instrument gets (TOP-BEND split, zone setup, note-on ordering)
    juce::MidiMessageSequence prepareMidiForInstrument (juce::MidiMessageSequence, juce::String& note) const;
    juce::String describeMidiSequence (const juce::MidiMessageSequence&) const;

    //== AI control (ControlServer; handler runs on the message thread) ==
    juce::var handleControl (const juce::var& request);
    juce::var controlStatus() const;
    juce::AudioProcessor* processorForRole (const juce::String& role) const;
    juce::MidiMessageSequence sequenceFromEvents (const juce::var& events, juce::String& error) const;
    // What a render of the mixer takes: AUDIO's file, the sounding INST tracks with the loaded MIDI file on their
    // channels, the master FX and strip (docs/TRACKS.md P4). `left` names what could not be taken, and why.
    // sounding: take the tracks as the live graph plays them now (the source-mode silence counts), not as the user set the mix;
    // audioWhenChain: take the AUDIO file even when it is the bounce of the MIDI file (the live graph plays that file)
    struct MixOptions { bool audio = true, insts = true, master = true, sounding = false, audioWhenChain = false; };
    juce::var buildMixRequest (const MixOptions&, const juce::var& request, juce::StringArray& left);
    juce::var startRenderMix (const juce::var& request);
    // The MIDI chain's bounce through the rig, for more than the in-process bounce can do (INST 2 / 3, channel masks).
    bool instrumentsNeedRig() const;
    bool startMidiMixBounce();
    bool multiBounceRunning = false;
    bool bounceRunning() const { return bounceEngine.isBouncing() || multiBounceRunning; }
    std::function<void (const juce::var&)> rigExtraDone;      // runs once when the current rig job ends   // render_mix: the mixer as it is now, offline through the rig
    juce::var startRigRender (const juce::var& request);   // rig_render: fixed inst -> insert -> master, offline
    juce::var startAraProbe (const juce::var& request);     // ara_probe: does this plug-in offer an ARA factory? (async)

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
    juce::File midiRecFile() const;
    juce::File autoBackendFile() const;
    juce::File inputPairFile() const;
    juce::File midiOutFile() const;
    juce::File midiThruFile() const;
    bool ensureOutputDevice();
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
    juce::File midiFolder;   // <exe dir>\MIDI - shared home of recorded takes and the open dialog
    MidiTakeRecorder midiRecorder;
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

    //== MPE / AI control state ==
    mpe::Config mpeConfig;
    juce::String mpeNote;                       // what the loaded MIDI file looked like
    juce::var araProbeResult;                   // last ara_probe result, shown in status
    bool araProbePending = false;
    RigWorkerClient rigWorker;                  // rig_render runs in a separate worker process
    juce::var rigResult;                        // last rig_render result, shown in status
    int pendingLoads = 0;                       // async plugin loads in flight (message thread)
    Graph::Node::Ptr tapNode;
    OutputTap* outputTap = nullptr;             // owned by the graph node

    //== mixer (docs/TRACKS.md): four tracks (AUDIO, INST 1-3), each a source -> strip, summed into the master ==
    // master: [FX insert] -> master strip -> tap -> device out. The legacy single FX is the master insert: with one
    // track sounding (the source modes still pick which) it does what it always did.
public:
    enum Track { trkAudio = 0, trkInst1 = 1, trkInst2 = 2, trkInst3 = 3, numTracks = 4 };
private:
    Graph::Node::Ptr stripNode[numTracks], masterStripNode, midiFilterNode[numTracks];
    TrackStrip* strips[numTracks] = {};          // owned by the graph nodes
    TrackStrip* masterStrip = nullptr;
    MidiChannelFilter* midiFilters[numTracks] = {};
    struct ExtraInstrument { Graph::Node::Ptr node; juce::String name; std::unique_ptr<juce::DocumentWindow> editor; };
    ExtraInstrument extraInst[2];                 // INST 2, INST 3
    // Effect slots (docs/TRACKS.md): one insert per track (source -> insert -> strip), and the FX of the two send buses.
    // A slot id: the insert of track t is slotInsertBase + t, the FX of send bus n (0 / 1) is slotBusBase + n.
public:
    enum { slotInsertBase = 100, slotBusBase = 200, tiBus1 = 10, tiBus2 = 11 };   // tiBus*: the return strips in track_set / track_status
private:
    struct FxSlot { Graph::Node::Ptr node; juce::String name; std::unique_ptr<juce::DocumentWindow> editor; };
    FxSlot insertSlot[numTracks];
    FxSlot busSlot[2];
    FxSlot& fxSlot (int slot)             { return slot >= slotBusBase ? busSlot[slot - slotBusBase] : insertSlot[slot - slotInsertBase]; }
    const FxSlot& fxSlot (int slot) const { return slot >= slotBusBase ? busSlot[slot - slotBusBase] : insertSlot[slot - slotInsertBase]; }
    Graph::Node::Ptr sendNode[numTracks][2], busReturnNode[2];   // post-fader send levels (-100 dB = off), the bus return strips
    TrackStrip* sendLevel[numTracks][2] = {};
    TrackStrip* busReturn[2] = {};
    void setSlotEffect (int slot, std::unique_ptr<juce::AudioPluginInstance>, const juce::PluginDescription&);
    void removeSlotEffect (int slot);
    static int slotFromRole (const juce::String& role);   // "insert_audio" .. "insert_inst3", "send1", "send2" -> a slot id, else -1
    bool slotsInUse() const;                               // any insert, any bus FX
    std::atomic<juce::AudioProcessor*> extraProc[2] { nullptr, nullptr };   // listener-thread-safe identity, like instrumentProc
    ExtraInstrument& extra (int track) { return extraInst[track - trkInst2]; }
    const ExtraInstrument& extra (int track) const { return extraInst[track - trkInst2]; }
    MidiScheduler midiScheduler { [this] (const juce::MidiMessage& m) { injectMidi (m); } };
    ControlServer controlServer { [this] (const juce::var& r) { return handleControl (r); } };
    int controlPort = 0;
    juce::File mpeFile() const;
    juce::File tracksFile() const;         // tracks.json: strip gain / balance and the INST MIDI channels (not mute / solo)
    void saveTracks() const;
    void loadTracks();
    juce::File controlPortFile() const;

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
        juce::String type;   // "float" / "int" / "bool" / "choice"
        std::unique_ptr<juce::Label>        label;
        std::unique_ptr<juce::Slider>       slider;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::ComboBox>     combo;
    };
    struct GpuModule
    {
        juce::String name;
        std::unique_ptr<juce::ToggleButton> enable;   // module on/off in the chain
        std::vector<GpuControl> controls;
    };
    std::vector<GpuModule> gpuModules;   // built from the worker's describe schema
    juce::Viewport  gpuViewport;         // the chain outgrew the window: scroll it
    juce::Component gpuPanelHolder;      // controls live here, inside the viewport
    int gpuPanelContentHeight() const;

    //== MIDI thru to hardware (off by default; the chosen output is remembered) ==
    std::unique_ptr<juce::MidiOutput> midiOut;
    juce::CriticalSection            midiOutLock;
    std::atomic<bool>                midiThru { false };

    int inputPairStart = 0;   // 0-based physical channel; remembered (live_input_pair.txt), 1/2 by default

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

    // MIDI bounce (Phase C) + take recorder
    juce::TextButton   openMidiButton { "Open MIDI file..." };
    juce::ToggleButton midiRecButton  { "MIDI REC" };
    juce::ToggleButton mpeButton      { "MPE" };
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

    juce::ToggleButton midiThruButton { "MIDI thru" };
    juce::ComboBox     midiOutCombo, inputPairCombo, recentCombo;
    juce::Label        midiOutLabel  { {}, "MIDI out:" };
    juce::Label        inputPairLabel{ {}, "Live input pair:" };
    juce::Label        recentLabel   { {}, "Cached plugins:" };
    juce::Label        pluginLabel;
    juce::Label        statusLabel;

    std::unique_ptr<juce::DocumentWindow> editorWindow, instEditorWindow;

    //== signal-flow diagram layout ==
    // paint() draws titled stage boxes + connecting arrows; resized() fills them.
    juce::Rectangle<int> boxSource, boxProcess, boxMixer, boxFx, boxOut;
    static constexpr int mixerHeight = 300;
    std::unique_ptr<MixerPanel> mixerPanel;
    void drawStageBox (juce::Graphics&, juce::Rectangle<int>, const juce::String& title,
                       juce::Colour accent) const;
    void drawFlowArrow (juce::Graphics&, juce::Point<int> from, juce::Point<int> to,
                        juce::Colour) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)

private:
    sn::KnobLookAndFeel snLaf;   // ★メンバの先頭=最後に壊れる(子より長生きさせる)
};
