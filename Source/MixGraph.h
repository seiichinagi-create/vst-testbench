#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <vector>

// The mixing topology, in one place: tracks -> (summed) -> master -> output.
//
//   track 0:  source -> insert -> insert ... \
//   track 1:  source -> insert ...            >--> master (first node = the summing input) -> ... -> output
//   track N:  ...                            /
//
// The offline rig (RigRender) builds its graph with this class, and the live bench is meant to do the same
// (docs/TRACKS.md), so that "what the bench plays" and "what the rig renders" are the same graph by construction
// and the null test between them means something.
//
// The class only wires and places; it does not process, render or own a thread. The graph owns the plug-ins.
class MixGraph
{
public:
    using Graph = juce::AudioProcessorGraph;

    struct Placed
    {
        Graph::Node::Ptr node;
        juce::String role;   // "source" / "inst" / "insert" / "master" (free text; reports only)
        int track;           // -1 = master
    };

    // The rig: `place` creates the nodes. `nonRealtime`: plug-ins are told they run offline. The graph's own play
    // config is the caller's.
    MixGraph (Graph& g, double sampleRate, int blockSize, bool nonRealtime)
        : graph (g), sr (sampleRate), block (blockSize), offline (nonRealtime), kind (Graph::UpdateKind::none)
    {
        // Every add* below runs with UpdateKind::none: one rebuild only, inside prepareToPlay. A queued async
        // rebuild on the message thread could otherwise interleave with it (the graph's latency read 0 in 2 of 10
        // renders: the rig's 2026-10 finding).
        outNode = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode),
                                 {}, kind);
    }

    // The live bench: the nodes (device in/out, plug-ins, file player, tap) exist already and stay; only the connections are
    // made here, with the graph's usual updates. `adopt` registers an existing node as a part of a track.
    MixGraph (Graph& g, Graph::Node::Ptr existingOutput, Graph::UpdateKind updateKind)
        : graph (g), sr (0.0), block (0), offline (false), kind (updateKind), outNode (existingOutput)
    {
    }

    // Takes the plug-in into the graph (all buses on, prepared for the play config). track -1 = master.
    Graph::Node::Ptr place (std::unique_ptr<juce::AudioProcessor> plugin, const juce::String& role, int track)
    {
        plugin->enableAllBuses();
        if (offline)
            plugin->setNonRealtime (true);
        plugin->setPlayConfigDetails (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels(), sr, block);
        auto node = graph.addNode (std::move (plugin), {}, kind);
        placedNodes.push_back ({ node, role, track });
        return node;
    }

    // An existing node as a part of a track (no node is created; nothing is prepared).
    Graph::Node::Ptr adopt (Graph::Node::Ptr node, const juce::String& role, int track)
    {
        placedNodes.push_back ({ node, role, track });
        return node;
    }

    // Audio from `from` into `to` (to == null: the output). Stereo; a mono output feeds both sides.
    // `fromBase`: the first channel of `from` (the live input's chosen pair).
    void connect (Graph::Node::Ptr from, Graph::Node::Ptr to, int fromBase = 0)
    {
        const int outs = from->getProcessor()->getTotalNumOutputChannels() - fromBase;
        const auto dest = to != nullptr ? to : outNode;
        const int ins = to != nullptr ? to->getProcessor()->getTotalNumInputChannels() : 2;
        for (int ch = 0; ch < juce::jmin (2, outs, ins); ++ch)
            graph.addConnection ({ { from->nodeID, fromBase + ch }, { dest->nodeID, ch } }, kind);
        if (outs == 1 && ins >= 2)
            graph.addConnection ({ { from->nodeID, fromBase }, { dest->nodeID, 1 } }, kind);
    }

    // The master chain, in series, the last one into the output. Its first node is where the tracks are summed
    // (with no master, the output is). Call before addTrack.
    void setMaster (const std::vector<Graph::Node::Ptr>& chain)
    {
        masterIn = chain.empty() ? Graph::Node::Ptr() : chain.front();
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            connect (chain[i], chain[i + 1]);
        if (! chain.empty())
            connect (chain.back(), nullptr);
    }

    // One track's chain in series, the last one into the summing input. `firstBase`: the first channel of the
    // chain's first node (the live input's chosen pair).
    void addTrack (const std::vector<Graph::Node::Ptr>& chain, int firstBase = 0)
    {
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            connect (chain[i], chain[i + 1], i == 0 ? firstBase : 0);
        connect (chain.back(), masterIn, chain.size() == 1 ? firstBase : 0);
        ++numTracks;
    }

    // MIDI into a node (a VSTi's MIDI input, from a feeder or from the host's MIDI input node).
    void connectMidi (Graph::Node::Ptr from, Graph::Node::Ptr to)
    {
        graph.addConnection ({ { from->nodeID, Graph::midiChannelIndex }, { to->nodeID, Graph::midiChannelIndex } }, kind);
    }

    Graph::Node::Ptr addNode (std::unique_ptr<juce::AudioProcessor> p) { return graph.addNode (std::move (p), {}, kind); }
    Graph::Node::Ptr output() const { return outNode; }
    Graph::Node::Ptr masterInput() const { return masterIn; }
    const std::vector<Placed>& placed() const { return placedNodes; }

    // What the graph's delay compensation should come to: the longest track plus the master, from the latencies the
    // plug-ins declare NOW. `perTrack` gets each track's own sum (one entry per track index seen).
    int declaredLatency (std::vector<int>& perTrack) const
    {
        perTrack.assign ((size_t) numTracks, 0);
        int masterLat = 0;
        for (auto& p : placedNodes)
        {
            const int l = p.node->getProcessor()->getLatencySamples();
            if (p.track < 0) masterLat += l; else perTrack[(size_t) p.track] += l;
        }
        const int longest = perTrack.empty() ? 0 : *std::max_element (perTrack.begin(), perTrack.end());
        return longest + masterLat;
    }

private:
    Graph& graph;
    double sr;
    int block;
    bool offline;
    Graph::UpdateKind kind;
    Graph::Node::Ptr outNode, masterIn;
    std::vector<Placed> placedNodes;
    int numTracks = 0;
};
