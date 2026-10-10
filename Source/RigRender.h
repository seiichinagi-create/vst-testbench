#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <algorithm>
#include <functional>

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

    // Plays a pre-loaded stereo buffer from sample 0, then silence. The source for feeding one render's
    // output into the next (a chain split in two must equal the chain rendered whole).
    class FileSource : public juce::AudioProcessor
    {
    public:
        explicit FileSource (std::shared_ptr<juce::AudioBuffer<float>> data)
            : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              buffer (std::move (data)) {}

        const juce::String getName() const override { return "FileSource"; }
        void prepareToPlay (double, int) override { pos = 0; }
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            b.clear();
            const int total = buffer->getNumSamples();
            const int n = juce::jmax (0, juce::jmin (b.getNumSamples(), total - pos));
            for (int c = 0; c < juce::jmin (b.getNumChannels(), buffer->getNumChannels()); ++c)
                b.copyFrom (c, 0, *buffer, c, pos, n);
            pos += b.getNumSamples();
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
        std::shared_ptr<juce::AudioBuffer<float>> buffer;
        int pos = 0;
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
        int block = 512;
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
        const auto none = Graph::UpdateKind::none;
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

        auto outNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode), {}, none);

        struct Placed { Graph::Node::Ptr node; juce::String role; int track; };   // track -1 = master
        std::vector<Placed> placed;
        auto place = [&] (Stage& st, int track)
        {
            st.plugin->enableAllBuses();
            st.plugin->setNonRealtime (true);
            st.plugin->setPlayConfigDetails (st.plugin->getTotalNumInputChannels(),
                                             st.plugin->getTotalNumOutputChannels(), sr, block);
            auto node = graph.addNode (std::move (st.plugin), {}, none);
            placed.push_back ({ node, st.role, track });
            return node;
        };
        // audio from `from` into `to` (to == null: the output node)
        auto connect = [&] (Graph::Node::Ptr from, Graph::Node::Ptr to)
        {
            const int outs = from->getProcessor()->getTotalNumOutputChannels();
            const auto dest = to != nullptr ? to : outNode;
            const int ins = to != nullptr ? to->getProcessor()->getTotalNumInputChannels() : 2;
            for (int ch = 0; ch < juce::jmin (2, outs, ins); ++ch)
                graph.addConnection ({ { from->nodeID, ch }, { dest->nodeID, ch } }, none);
            if (outs == 1 && ins >= 2)       // mono into stereo: feed both sides
                graph.addConnection ({ { from->nodeID, 0 }, { dest->nodeID, 1 } }, none);
        };

        std::vector<Graph::Node::Ptr> masterNodes;
        for (auto& st : spec.master)
            masterNodes.push_back (place (st, -1));
        const Graph::Node::Ptr masterIn = masterNodes.empty() ? Graph::Node::Ptr() : masterNodes.front();
        for (size_t i = 0; i + 1 < masterNodes.size(); ++i)
            connect (masterNodes[i], masterNodes[i + 1]);
        if (! masterNodes.empty())
            connect (masterNodes.back(), nullptr);

        std::vector<int> trackLatency (spec.tracks.size(), 0);
        Graph::Node::Ptr firstSource;
        bool firstTrackHasMore = false;
        for (size_t ti = 0; ti < spec.tracks.size(); ++ti)
        {
            auto& tr = spec.tracks[ti];
            std::vector<Graph::Node::Ptr> chain;
            for (auto& st : tr.stages)
                chain.push_back (place (st, (int) ti));

            const bool midiTrack = tr.impulseAt < 0 && tr.sourceSamples < 0;
            if (midiTrack)
            {
                auto feeder = graph.addNode (std::make_unique<MidiFeeder> (tr.sequence, sr), {}, none);
                graph.addConnection ({ { feeder->nodeID, Graph::midiChannelIndex },
                                       { chain[0]->nodeID, Graph::midiChannelIndex } }, none);
            }
            for (size_t i = 0; i + 1 < chain.size(); ++i)
                connect (chain[i], chain[i + 1]);
            connect (chain.back(), masterIn);        // summed with the other tracks at the master's input (or the output)

            if (ti == 0) { firstSource = chain[0]; firstTrackHasMore = chain.size() > 1 || ! masterNodes.empty(); }
        }

        if (spec.dryParallel && firstSource != nullptr && firstTrackHasMore)
            connect (firstSource, nullptr);

        // The graph builds its delay compensation inside prepareToPlay from the latencies the plugins declare
        // THEN. A plugin that declares late (Legacy Distortion, after an async capture load) can leave the graph
        // with 0 while the plugins say 4: the dry path then goes uncompensated and the render is wrong without
        // any error. So: settle, compare the graph's total with what the declared latencies add up to (the
        // longest track plus the master), and if they differ drop the sequence (releaseResources) and prepare
        // again. Never render on a mismatch.
        auto declaredTotal = [&] ()
        {
            std::fill (trackLatency.begin(), trackLatency.end(), 0);
            int masterLat = 0;
            for (auto& p : placed)
            {
                const int l = p.node->getProcessor()->getLatencySamples();
                if (p.track < 0) masterLat += l; else trackLatency[(size_t) p.track] += l;
            }
            return *std::max_element (trackLatency.begin(), trackLatency.end()) + masterLat;
        };
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

        while (pos < total && ! threadShouldExit())
        {
            const int n = (int) juce::jmin ((juce::int64) block, total - pos);

            midi.clear();
            buf.clear();
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, 0, n);
            graph.processBlock (view, midi);

            // drop the compensated head
            const int skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, discard - pos);
            const int keep = n - skip;
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
        finish (r);
    }

    Spec spec;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigRender)
};
