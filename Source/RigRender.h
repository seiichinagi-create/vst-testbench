#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <functional>

//==============================================================================
// Fixed-topology test rig, rendered offline and headless:
//
//   MIDI -> VSTi -> [insert FX] -> [master FX] -> wav
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
    struct Stage
    {
        juce::String role;   // "inst" / "insert" / "master"
        std::unique_ptr<juce::AudioPluginInstance> plugin;
    };

    struct Spec
    {
        std::vector<Stage> stages;            // in signal order; stages[0] is the instrument
        juce::MidiMessageSequence sequence;   // seconds
        double sampleRate = 48000.0;
        int block = 512;
        double tailSeconds = 2.0;
        bool compensate = true;
        int settleMs = 0;                     // wait after prepareToPlay so async plugin work (capture loads) can land
        juce::File out;
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

        if (spec.stages.empty() || spec.stages[0].plugin == nullptr) { fail ("no instrument"); return; }
        if (spec.sequence.getNumEvents() == 0)                       { fail ("no MIDI events"); return; }

        const double sr = spec.sampleRate;
        const int block = spec.block;
        const juce::int64 body = (juce::int64) ((spec.sequence.getEndTime() + spec.tailSeconds) * sr);
        if (body > 150'000'000) { fail ("too long to render"); return; }

        //-- build the graph: MIDI -> inst -> insert -> master -> out ---------
        Graph graph;
        graph.setPlayConfigDetails (0, 2, sr, block);
        graph.setNonRealtime (true);

        auto outNode  = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode));
        auto midiNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::midiInputNode));

        std::vector<Graph::Node::Ptr> nodes;
        std::vector<juce::String> roles;
        for (auto& st : spec.stages)
        {
            roles.push_back (st.role);
            st.plugin->enableAllBuses();
            st.plugin->setNonRealtime (true);
            st.plugin->setPlayConfigDetails (st.plugin->getTotalNumInputChannels(),
                                             st.plugin->getTotalNumOutputChannels(), sr, block);
            nodes.push_back (graph.addNode (std::move (st.plugin)));
        }

        graph.addConnection ({ { midiNode->nodeID, Graph::midiChannelIndex },
                               { nodes[0]->nodeID, Graph::midiChannelIndex } });

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const int outs = nodes[i]->getProcessor()->getTotalNumOutputChannels();
            const auto dest = i + 1 < nodes.size() ? nodes[i + 1] : outNode;
            const int ins = i + 1 < nodes.size() ? dest->getProcessor()->getTotalNumInputChannels() : 2;
            for (int ch = 0; ch < juce::jmin (2, outs, ins); ++ch)
                graph.addConnection ({ { nodes[i]->nodeID, ch }, { dest->nodeID, ch } });
            // mono source into a stereo destination: feed both sides
            if (outs == 1 && ins >= 2)
                graph.addConnection ({ { nodes[i]->nodeID, 0 }, { dest->nodeID, 1 } });
        }

        graph.prepareToPlay (sr, block);
        if (spec.settleMs > 0)
            wait (spec.settleMs);   // the message thread is free meanwhile: async updates run now, not mid-render
        const int graphLatency = graph.getLatencySamples();

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
        put (r, "compensated", spec.compensate);
        put (r, "peak_db", peak > 1.0e-6 ? 20.0 * std::log10 (peak) : -120.0);
        finish (r);
    }

    Spec spec;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigRender)
};
