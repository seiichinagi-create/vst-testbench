#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
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

    struct Stage
    {
        juce::String role;   // "inst" / "source" / "insert" / "master"
        std::unique_ptr<juce::AudioProcessor> plugin;
    };

    struct Spec
    {
        std::vector<Stage> stages;            // in signal order; stages[0] is the instrument (or an ImpulseSource)
        juce::int64 impulseAt = -1;           // >= 0: stages[0] is an ImpulseSource at this sample; MIDI is not used
        juce::MidiMessageSequence sequence;   // seconds
        double sampleRate = 48000.0;
        int block = 512;
        double tailSeconds = 2.0;
        bool compensate = true;
        bool dryParallel = false;             // also send the instrument straight to the output: a second, shorter path for the graph's PDC to align
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

        if (spec.stages.empty() || spec.stages[0].plugin == nullptr) { fail ("no instrument"); return; }
        const bool impulse = spec.impulseAt >= 0;
        if (! impulse && spec.sequence.getNumEvents() == 0)          { fail ("no MIDI events"); return; }

        const double sr = spec.sampleRate;
        const int block = spec.block;
        const juce::int64 body = impulse ? spec.impulseAt + (juce::int64) (spec.tailSeconds * sr)
                                         : (juce::int64) ((spec.sequence.getEndTime() + spec.tailSeconds) * sr);
        if (body > 150'000'000) { fail ("too long to render"); return; }

        //-- build the graph: MIDI -> inst -> insert -> master -> out ---------
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

        auto outNode  = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode), {}, none);
        auto midiNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::midiInputNode), {}, none);

        std::vector<Graph::Node::Ptr> nodes;
        std::vector<juce::String> roles;
        for (auto& st : spec.stages)
        {
            roles.push_back (st.role);
            st.plugin->enableAllBuses();
            st.plugin->setNonRealtime (true);
            st.plugin->setPlayConfigDetails (st.plugin->getTotalNumInputChannels(),
                                             st.plugin->getTotalNumOutputChannels(), sr, block);
            nodes.push_back (graph.addNode (std::move (st.plugin), {}, none));
        }

        if (! impulse)
            graph.addConnection ({ { midiNode->nodeID, Graph::midiChannelIndex },
                                   { nodes[0]->nodeID, Graph::midiChannelIndex } }, none);

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const int outs = nodes[i]->getProcessor()->getTotalNumOutputChannels();
            const auto dest = i + 1 < nodes.size() ? nodes[i + 1] : outNode;
            const int ins = i + 1 < nodes.size() ? dest->getProcessor()->getTotalNumInputChannels() : 2;
            for (int ch = 0; ch < juce::jmin (2, outs, ins); ++ch)
                graph.addConnection ({ { nodes[i]->nodeID, ch }, { dest->nodeID, ch } }, none);
            // mono source into a stereo destination: feed both sides
            if (outs == 1 && ins >= 2)
                graph.addConnection ({ { nodes[i]->nodeID, 0 }, { dest->nodeID, 1 } }, none);
        }

        if (spec.dryParallel && nodes.size() > 1)
        {
            const int outs = nodes[0]->getProcessor()->getTotalNumOutputChannels();
            for (int ch = 0; ch < juce::jmin (2, outs); ++ch)
                graph.addConnection ({ { nodes[0]->nodeID, ch }, { outNode->nodeID, ch } }, none);
            if (outs == 1)
                graph.addConnection ({ { nodes[0]->nodeID, 0 }, { outNode->nodeID, 1 } }, none);
        }

        // The graph builds its delay compensation inside prepareToPlay from the latencies the plugins declare
        // THEN. A plugin that declares late (Legacy Distortion, after an async capture load) can leave the graph
        // with 0 while the plugins say 4: the dry path then goes uncompensated and the render is wrong without
        // any error. So: settle, compare the graph's total with the sum of the declared latencies, and if they
        // differ drop the sequence (releaseResources) and prepare again. Never render on a mismatch.
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
            declaredSum = 0;
            for (auto& n : nodes)
                declaredSum += n->getProcessor()->getLatencySamples();
            if (graphLatency == declaredSum)
                break;
        }
        if (graphLatency != declaredSum)
        {
            graph.releaseResources();
            fail ("graph latency " + juce::String (graphLatency) + " never matched the declared sum "
                  + juce::String (declaredSum) + " after " + juce::String (attempts) + " tries");
            return;
        }

        //-- declared latency, read now that everything is prepared -----------
        auto stagesInfo = juce::Array<juce::var>();
        int chainLatency = 0;
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            auto* p = nodes[i]->getProcessor();
            auto s = obj();
            put (s, "role", roles[i]);
            put (s, "name", p->getName());
            put (s, "latency", p->getLatencySamples());
            stagesInfo.add (s);
            chainLatency += p->getLatencySamples();
        }

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
        juce::MidiBuffer midi;
        int evIndex = 0;
        juce::int64 pos = 0, written = 0;
        double peak = 0.0;
        bool ok = true;

        while (pos < total && ! threadShouldExit())
        {
            const int n = (int) juce::jmin ((juce::int64) block, total - pos);

            midi.clear();
            const double blockEnd = (double) (pos + n) / sr;
            while (evIndex < spec.sequence.getNumEvents())
            {
                const auto* ev = spec.sequence.getEventPointer (evIndex);
                const double t = ev->message.getTimeStamp();
                if (t >= blockEnd) break;
                midi.addEvent (ev->message, juce::jlimit (0, n - 1, (int) ((juce::int64) (t * sr) - pos)));
                ++evIndex;
            }

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
