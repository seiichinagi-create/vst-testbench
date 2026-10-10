#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <algorithm>
#include <functional>
#include "MixGraph.h"

//==============================================================================
// Fixed-topology test rig, rendered offline and headless:
//
//   MIDI -> VSTi -> [insert FX] -> [master FX] -> wav
//   (dryParallel: the VSTi also goes straight to the output, so the graph must delay it by the chain's latency)
//
// A throw-away AudioProcessorGraph is built for every render, so no state is
// carried over between runs and the result depends only on the plugins, their
// states and the MIDI events. It never touches the live graph or the audio
// device. The caller creates the plugin instances (message thread) and hands
// them over; this thread prepares and runs them, like MidiBounceEngine.
//
// Latency is read AFTER prepareToPlay and the state load (a plugin may declare
// a latency that depends on its state). With `compensate` the declared total is
// dropped from the head of the wav, as a host with plugin delay compensation
// would, so the file lines up with the MIDI timeline.
//==============================================================================
class RigRender : private juce::Thread
{
public:
    // One unit impulse (a single non-zero sample) at a known position, stereo, no input, no MIDI.
    // The source for measuring what a plugin really delays, as opposed to what it declares.
    class ImpulseSource : public juce::AudioProcessor
    {
    public:
        ImpulseSource (juce::int64 atSample, float amplitude)
            : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              at (atSample), amp (amplitude) {}

        const juce::String getName() const override { return "Impulse"; }
        void prepareToPlay (double, int) override { pos = 0; }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            b.clear();
            const auto n = (juce::int64) b.getNumSamples();
            if (at >= pos && at < pos + n)
                for (int c = 0; c < b.getNumChannels(); ++c)
                    b.setSample (c, (int) (at - pos), amp);
            pos += n;
        }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        juce::int64 at, pos = 0;
        float amp;
    };

    // An audio clip on a track: `length` samples of a pre-loaded stereo buffer, starting at `offset` in the file, placed
    // at `start` on the timeline, times a gain; silence everywhere else. The source for feeding one render's output into
    // the next (a chain split in two must equal the chain rendered whole) and the model of an ARA playback region.
    class FileSource : public juce::AudioProcessor
    {
    public:
        FileSource (std::shared_ptr<juce::AudioBuffer<float>> data, juce::int64 startOnTimeline = 0,
                    juce::int64 offsetInFile = 0, juce::int64 lengthInSamples = -1, float gainLinear = 1.0f)
            : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              buffer (std::move (data)), start (startOnTimeline), offset (offsetInFile), gain (gainLinear)
        {
            const juce::int64 inFile = (juce::int64) buffer->getNumSamples() - offset;
            length = lengthInSamples < 0 ? inFile : juce::jmin (lengthInSamples, inFile);
            length = juce::jmax<juce::int64> (0, length);
        }

        // where the clip ends on the timeline
        juce::int64 endOnTimeline() const { return start + length; }

        const juce::String getName() const override { return "FileSource"; }
        void prepareToPlay (double, int) override { pos = 0; }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            b.clear();
            const juce::int64 n = b.getNumSamples();
            const juce::int64 from = juce::jmax (pos, start);                  // overlap of [pos, pos+n) with the clip
            const juce::int64 to = juce::jmin (pos + n, start + length);
            if (to > from)
                for (int c = 0; c < juce::jmin (b.getNumChannels(), buffer->getNumChannels()); ++c)
                    b.copyFromWithRamp (c, (int) (from - pos), buffer->getReadPointer (c) + (offset + (from - start)),
                                        (int) (to - from), gain, gain);
            pos += n;
        }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return "Default"; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        std::shared_ptr<juce::AudioBuffer<float>> buffer;
        juce::int64 start = 0, offset = 0, length = 0, pos = 0;
        float gain = 1.0f;
    };

    // Delays by `actual` samples and DECLARES `declared`. A device whose true behaviour is known, to check
    // that the latency measurement sees a wrong declaration (a test that cannot fail proves nothing).
    class KnownDelay : public juce::AudioProcessor
    {
    public:
        KnownDelay (int actualSamples, int declaredSamples)
            : juce::AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              actual (juce::jmax (0, actualSamples)), declared (declaredSamples) {}

        const juce::String getName() const override { return "KnownDelay"; }
        void prepareToPlay (double, int) override
        {
            ring.assign ((size_t) (2 * actual), 0.0f);
            head = 0;
            setLatencySamples (declared);
        }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            if (actual == 0)
                return;
            for (int c = 0; c < juce::jmin (2, b.getNumChannels()); ++c)
            {
                float* d = b.getWritePointer (c);
                float* r = ring.data() + (size_t) c * (size_t) actual;
                int h = head;
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const float out = r[h];
                    r[h] = d[i];
                    d[i] = out;
                    h = (h + 1) % actual;
                }
            }
            head = (head + b.getNumSamples()) % actual;
        }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        int actual, declared, head = 0;
        std::vector<float> ring;
    };

    // Feeds a MIDI sequence (seconds) to whatever its MIDI output is connected to. One per MIDI track, so that
    // several instruments each get their own part (the graph's single MIDI input would send all of them the same).
    class MidiFeeder : public juce::AudioProcessor
    {
    public:
        MidiFeeder (juce::MidiMessageSequence s, double rate)
            : juce::AudioProcessor (BusesProperties()), seq (std::move (s)), sr (rate) {}

        const juce::String getName() const override { return "MidiFeeder"; }
        void prepareToPlay (double, int) override { pos = 0; index = 0; }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override
        {
            midi.clear();
            const int n = b.getNumSamples();
            const double blockEnd = (double) (pos + n) / sr;
            while (index < seq.getNumEvents())
            {
                const auto* ev = seq.getEventPointer (index);
                const double t = ev->message.getTimeStamp();
                if (t >= blockEnd)
                    break;
                midi.addEvent (ev->message, juce::jlimit (0, juce::jmax (0, n - 1), (int) ((juce::int64) (t * sr) - pos)));
                ++index;
            }
            pos += n;
        }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return true; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        juce::MidiMessageSequence seq;
        double sr;
        juce::int64 pos = 0;
        int index = 0;
    };

    // Exponentially decaying tail of a KNOWN length: y[n] = x[n] + a*y[n-1], -60 dB after `t60` seconds, and a
    // tail length it DECLARES (which may be a lie). The control for the tail check.
    class KnownTail : public juce::AudioProcessor
    {
    public:
        KnownTail (double t60Seconds, double declaredSeconds)
            : juce::AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              t60 (t60Seconds), declared (declaredSeconds) {}

        const juce::String getName() const override { return "KnownTail"; }
        void prepareToPlay (double rate, int) override
        {
            a = (float) std::pow (10.0, -3.0 / (t60 * rate));
            y[0] = y[1] = 0.0f;
        }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            for (int c = 0; c < juce::jmin (2, b.getNumChannels()); ++c)
            {
                float* d = b.getWritePointer (c);
                float s = y[c];
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    s = d[i] + a * s;
                    d[i] = s;
                }
                y[c] = s;
            }
        }
        double getTailLengthSeconds() const override { return declared; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        double t60, declared;
        float a = 0.0f, y[2] = { 0.0f, 0.0f };
    };

    // What the host tells a plug-in about the transport: tempo, time signature, position. One per render; the render
    // thread moves `pos` before every block.
    class RigPlayHead : public juce::AudioPlayHead
    {
    public:
        double bpm = 120.0, sampleRate = 48000.0;
        int sigNum = 4, sigDen = 4;
        juce::int64 pos = 0;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo i;
            i.setBpm (bpm);
            i.setTimeSignature (juce::AudioPlayHead::TimeSignature { sigNum, sigDen });
            i.setTimeInSamples (pos);
            i.setTimeInSeconds ((double) pos / sampleRate);
            i.setPpqPosition ((double) pos / sampleRate * bpm / 60.0);
            i.setIsPlaying (true);
            i.setIsRecording (false);
            i.setIsLooping (false);
            return i;
        }
    };

    // Writes what the host says about the transport into the audio, so a test can read it back:
    // left = bpm / 1000, right = ppq position / 1000 (advancing per sample), both -1 when the host says nothing.
    class PlayheadProbe : public juce::AudioProcessor
    {
    public:
        PlayheadProbe()
            : juce::AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)) {}
        const juce::String getName() const override { return "PlayheadProbe"; }
        void prepareToPlay (double rate, int) override { sr = rate; }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            b.clear();
            double bpm = -1000.0, ppq0 = -1000.0;
            if (auto* ph = getPlayHead())
                if (auto pi = ph->getPosition())
                {
                    if (auto t = pi->getBpm()) bpm = *t;
                    if (auto q = pi->getPpqPosition()) ppq0 = *q;
                }
            const double perSample = bpm > 0 ? bpm / 60.0 / sr : 0.0;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                b.setSample (0, i, (float) (bpm / 1000.0));
                if (b.getNumChannels() > 1)
                    b.setSample (1, i, (float) ((ppq0 + perSample * i) / 1000.0));
            }
        }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return "Default"; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    private:
        double sr = 48000.0;
    };

    // Input times a gain that is a host parameter (named "gain", 0..1, default 0). With a constant input, the output
    // shows at which sample a parameter change took effect.
    class GainProbe : public juce::AudioProcessor
    {
    public:
        GainProbe()
            : juce::AudioProcessor (BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
            addParameter (gain = new juce::AudioParameterFloat ("gain", "gain", 0.0f, 1.0f, 0.0f));
        }
        const juce::String getName() const override { return "GainProbe"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { b.applyGain (gain->get()); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return "Default"; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    private:
        juce::AudioParameterFloat* gain = nullptr;
    };

    struct Stage
    {
        juce::String role;   // "source" / "inst" / "insert" / "master"
        std::unique_ptr<juce::AudioProcessor> plugin;
    };

    // One track: a source (a VSTi fed by its own MIDI, an impulse, or a file) and its insert chain.
    struct Track
    {
        std::vector<Stage> stages;            // signal order; stages[0] is the source
        juce::int64 impulseAt = -1;           // >= 0: stages[0] is an ImpulseSource at this sample; no MIDI
        juce::int64 sourceSamples = -1;       // >= 0: stages[0] is a FileSource of this many samples; no MIDI
        juce::MidiMessageSequence sequence;   // seconds, for a VSTi source
    };

    // tracks -> (summed) -> master -> out. The graph delays every track by what the longest one needs.
    struct Spec
    {
        std::vector<Track> tracks;
        std::vector<Stage> master;
        double sampleRate = 48000.0;
        int block = 512;                      // the largest block; also what the plug-ins are prepared for
        std::vector<int> blockPattern;        // block lengths used in turn (each clamped to 1..block); empty = always `block`
        double bpm = 120.0;                   // the transport the plug-ins are told about
        int sigNum = 4, sigDen = 4;
        // Parameter changes at sample positions on the timeline (before latency compensation).
        struct AutomationPoint { juce::int64 sample; float value; };
        struct Automation { juce::AudioProcessorParameter* param = nullptr; std::vector<AutomationPoint> points; };
        std::vector<Automation> automation;
        bool automationBlockQuantised = false; // true: a change takes effect at the start of the block it falls in (what a simple host does)
        double tailSeconds = 2.0;
        bool compensate = true;
        bool dryParallel = false;             // also send track 0's source straight to the output: a second, shorter path for the graph's PDC to align
        int settleMs = 0;                     // wait after prepareToPlay so async plugin work (capture loads) can land
        juce::File out;
        std::function<void (const juce::String&)> progress;   // phase notes, from the render thread
    };

    RigRender() : juce::Thread ("rig-render") {}
    ~RigRender() override { stopThread (10000); }

    // Fires on the message thread with a result object (ok / error / numbers).
    std::function<void (juce::var)> onDone;

    bool isRunning() const { return isThreadRunning(); }

    void start (Spec s)
    {
        stopThread (10000);
        spec = std::move (s);
        startThread (juce::Thread::Priority::low);
    }

private:
    static juce::var obj() { return juce::var (new juce::DynamicObject()); }
    static void put (juce::var& o, const char* k, const juce::var& v) { o.getDynamicObject()->setProperty (k, v); }

    void finish (juce::var result)
    {
        if (threadShouldExit())
            return;
        auto cb = onDone;
        juce::MessageManager::callAsync ([cb, result] { if (cb != nullptr) cb (result); });
    }

    void fail (const juce::String& why)
    {
        auto r = obj();
        put (r, "ok", false);
        put (r, "error", why);
        finish (r);
    }

    void run() override
    {
        using Graph = juce::AudioProcessorGraph;
        auto note = [this] (const juce::String& m) { if (spec.progress != nullptr) spec.progress (m); };

        if (spec.tracks.empty()) { fail ("no tracks"); return; }
        for (auto& t : spec.tracks)
        {
            if (t.stages.empty() || t.stages[0].plugin == nullptr) { fail ("a track has no source"); return; }
            if (t.impulseAt < 0 && t.sourceSamples < 0 && t.sequence.getNumEvents() == 0) { fail ("a MIDI track has no events"); return; }
        }

        const double sr = spec.sampleRate;
        const int block = spec.block;
        juce::int64 longest = 0;
        for (auto& t : spec.tracks)
            longest = juce::jmax (longest, t.impulseAt >= 0 ? t.impulseAt
                                         : t.sourceSamples >= 0 ? t.sourceSamples
                                         : (juce::int64) (t.sequence.getEndTime() * sr));
        const juce::int64 body = longest + (juce::int64) (spec.tailSeconds * sr);
        if (body > 150'000'000) { fail ("too long to render"); return; }

        //-- build the graph: [MIDI ->] source -> inserts   (per track)  ->  master -> out -----------
        // One rebuild only, inside prepareToPlay: every add* below would otherwise queue an async rebuild
        // on the message thread that can interleave with it (graph latency read 0 in 2 of 10 renders).
        note ("building the graph");
        // The graph owns the plugin instances. Plugins must die on the message thread (some VST3s crash
        // otherwise: the bench fell over after about ten renders), so the last reference is handed to it.
        auto graphOwner = std::make_shared<Graph>();
        Graph& graph = *graphOwner;
        struct DieOnMessageThread
        {
            std::shared_ptr<Graph> g;
            ~DieOnMessageThread() { juce::MessageManager::callAsync ([g = std::move (g)] {}); }
        } deferredDestruction { graphOwner };
        graph.setPlayConfigDetails (0, 2, sr, block);
        graph.setNonRealtime (true);

        // The topology (placing, wiring, summing at the master) is MixGraph's: the live bench builds the same graph.
        MixGraph mix (graph, sr, block, /*nonRealtime*/ true);
        const auto& placed = mix.placed();

        std::vector<Graph::Node::Ptr> masterNodes;
        for (auto& st : spec.master)
            masterNodes.push_back (mix.place (std::move (st.plugin), st.role, -1));
        mix.setMaster (masterNodes);

        Graph::Node::Ptr firstSource;
        bool firstTrackHasMore = false;
        for (size_t ti = 0; ti < spec.tracks.size(); ++ti)
        {
            auto& tr = spec.tracks[ti];
            std::vector<Graph::Node::Ptr> chain;
            for (auto& st : tr.stages)
                chain.push_back (mix.place (std::move (st.plugin), st.role, (int) ti));

            const bool midiTrack = tr.impulseAt < 0 && tr.sourceSamples < 0;
            if (midiTrack)
                mix.connectMidi (mix.addNode (std::make_unique<MidiFeeder> (tr.sequence, sr)), chain[0]);
            mix.addTrack (chain);        // in series, the last one summed with the other tracks at the master's input (or the output)

            if (ti == 0) { firstSource = chain[0]; firstTrackHasMore = chain.size() > 1 || ! masterNodes.empty(); }
        }

        if (spec.dryParallel && firstSource != nullptr && firstTrackHasMore)
            mix.connect (firstSource, nullptr);

        // The graph builds its delay compensation inside prepareToPlay from the latencies the plugins declare
        // THEN. A plugin that declares late (Legacy Distortion, after an async capture load) can leave the graph
        // with 0 while the plugins say 4: the dry path then goes uncompensated and the render is wrong without
        // any error. So: settle, compare the graph's total with what the declared latencies add up to (the
        // longest track plus the master), and if they differ drop the sequence (releaseResources) and prepare
        // again. Never render on a mismatch.
        std::vector<int> trackLatency;
        auto declaredTotal = [&] () { return mix.declaredLatency (trackLatency); };
        int graphLatency = 0, declaredSum = 0, attempts = 0;
        for (; attempts < 5; ++attempts)
        {
            if (attempts > 0)
                graph.releaseResources();
            note ("prepareToPlay, attempt " + juce::String (attempts + 1));
            graph.prepareToPlay (sr, block);
            if (spec.settleMs > 0)
                wait (spec.settleMs);   // the message thread is free meanwhile: async updates run now, not mid-render
            graphLatency = graph.getLatencySamples();
            declaredSum = declaredTotal();
            if (graphLatency == declaredSum)
                break;
        }
        if (graphLatency != declaredSum)
        {
            graph.releaseResources();
            fail ("graph latency " + juce::String (graphLatency) + " never matched the declared total "
                  + juce::String (declaredSum) + " after " + juce::String (attempts) + " tries");
            return;
        }

        //-- declared latency and tail, read now that everything is prepared ----
        auto stagesInfo = juce::Array<juce::var>();
        for (auto& p : placed)
        {
            auto* proc = p.node->getProcessor();
            auto s = obj();
            put (s, "role", p.role);
            put (s, "track", p.track);
            put (s, "name", proc->getName());
            put (s, "latency", proc->getLatencySamples());
            put (s, "tail", proc->getTailLengthSeconds());
            stagesInfo.add (s);
        }
        juce::Array<juce::var> trackLatencies;
        for (int l : trackLatency)
            trackLatencies.add (l);
        const int chainLatency = declaredSum;

        note ("rendering");
        //-- write the wav -----------------------------------------------------
        spec.out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (spec.out.createOutputStream());
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (stream != nullptr)
            writer = wavFormat.createWriterFor (stream,
                         juce::AudioFormatWriterOptions{}
                             .withSampleRate (sr).withNumChannels (2).withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) { graph.releaseResources(); fail ("cannot write " + spec.out.getFileName()); return; }

        const juce::int64 total = body + chainLatency;     // the head that gets dropped comes on top
        const juce::int64 discard = spec.compensate ? chainLatency : 0;
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;                              // the feeders make the MIDI inside the graph
        juce::int64 pos = 0, written = 0;
        double peak = 0.0;
        bool ok = true;

        RigPlayHead playHead;
        playHead.bpm = spec.bpm; playHead.sigNum = spec.sigNum; playHead.sigDen = spec.sigDen; playHead.sampleRate = sr;
        graph.setPlayHead (&playHead);

        struct Event { juce::int64 sample; juce::AudioProcessorParameter* param; float value; };
        std::vector<Event> events;
        for (auto& a : spec.automation)
            for (auto& pt : a.points)
                events.push_back ({ pt.sample, a.param, pt.value });
        std::stable_sort (events.begin(), events.end(), [] (const Event& x, const Event& y) { return x.sample < y.sample; });
        size_t evIdx = 0, patIdx = 0;
        long blocks = 0;
        int blockMin = block, blockMax = 0;

        while (pos < total && ! threadShouldExit())
        {
            juce::int64 n = block;
            if (! spec.blockPattern.empty())
                n = juce::jlimit (1, block, spec.blockPattern[patIdx++ % spec.blockPattern.size()]);
            n = juce::jmin (n, total - pos);

            // Sample-accurate automation: apply the changes that are due, THEN end the block where the next one falls,
            // so a change always lands exactly on a block start. (Deciding the cut before applying left a block with a
            // later change inside it whenever the block was longer than the gap: found with a 4096-sample block.)
            // The quantised mode is the control: it applies a change at the start of the block it falls in.
            if (spec.automationBlockQuantised)
            {
                while (evIdx < events.size() && events[evIdx].sample < pos + n)
                {
                    events[evIdx].param->setValue (events[evIdx].value);
                    ++evIdx;
                }
            }
            else
            {
                while (evIdx < events.size() && events[evIdx].sample <= pos)
                {
                    events[evIdx].param->setValue (events[evIdx].value);
                    ++evIdx;
                }
                if (evIdx < events.size() && events[evIdx].sample < pos + n)
                    n = events[evIdx].sample - pos;
            }

            ++blocks;
            blockMin = juce::jmin (blockMin, (int) n);
            blockMax = juce::jmax (blockMax, (int) n);
            playHead.pos = pos;

            midi.clear();
            buf.clear();
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, 0, (int) n);
            graph.processBlock (view, midi);

            // drop the compensated head
            const int skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, discard - pos);
            const int keep = (int) n - skip;
            if (keep > 0)
            {
                const float* chans[2] = { buf.getReadPointer (0) + skip, buf.getReadPointer (1) + skip };
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < keep; ++i)
                        peak = juce::jmax (peak, (double) std::abs (chans[c][i]));
                if (! writer->writeFromFloatArrays (chans, 2, keep)) { ok = false; break; }
                written += keep;
            }
            pos += n;
        }

        writer.reset();
        graph.releaseResources();

        if (threadShouldExit()) return;
        if (! ok) { fail ("disk write failed"); return; }

        auto r = obj();
        put (r, "ok", true);
        put (r, "path", spec.out.getFullPathName());
        put (r, "sample_rate", sr);
        put (r, "samples", (juce::int64) written);
        put (r, "stages", stagesInfo);
        put (r, "track_latencies", trackLatencies);
        put (r, "chain_latency", chainLatency);
        put (r, "graph_latency", graphLatency);
        put (r, "prepare_attempts", attempts + 1);
        put (r, "compensated", spec.compensate);
        put (r, "dry_parallel", spec.dryParallel);
        put (r, "peak_db", peak > 1.0e-6 ? 20.0 * std::log10 (peak) : -120.0);
        put (r, "blocks", (juce::int64) blocks);
        put (r, "block_min", blockMin);
        put (r, "block_max", blockMax);
        put (r, "bpm", spec.bpm);
        finish (r);
    }

    Spec spec;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigRender)
};
