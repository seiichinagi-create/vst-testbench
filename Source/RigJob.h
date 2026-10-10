#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "RigRender.h"
#include "EventSequence.h"

//==============================================================================
// Job description shared by the bench (which writes it) and the rig worker process (which runs it).
//
//   { rate, block, tail, compensate, dry_parallel, settle, out,
//     tracks: [ { source: "midi" | "impulse" | "file", events, impulse_at, impulse_amp, source_path,
//                 stages: [ stage... ] } ... ],
//     master: [ stage... ] }
//
//   stage = { role, kind: "plugin", desc_xml, state, params }       plugin by PluginDescription XML
//         | { role, kind: "delay", delay_actual, delay_declared }   test double: a delay with a declared value
//         | { role, kind: "tail",  tail_t60, tail_declared }        test double: a decaying tail with a declared value
//
// The bench resolves names to descriptions (it owns the plugin cache); the worker only instantiates.
//==============================================================================
namespace rigjob
{
    inline bool buildStage (const juce::var& st, juce::AudioPluginFormatManager& formats, const RigRender::Spec& spec,
                            RigRender::Stage& out, juce::String& error,
                            const std::function<void (const juce::String&)>& note)
    {
        const auto role = st["role"].toString();
        const auto kind = st["kind"].toString();

        if (kind == "delay")
        {
            const int actual = (int) st["delay_actual"];
            out = { role, std::make_unique<RigRender::KnownDelay> (actual, st.hasProperty ("delay_declared") ? (int) st["delay_declared"] : actual) };
            return true;
        }
        if (kind == "tail")
        {
            const double t60 = (double) st["tail_t60"];
            out = { role, std::make_unique<RigRender::KnownTail> (t60, st.hasProperty ("tail_declared") ? (double) st["tail_declared"] : t60) };
            return true;
        }

        std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (st["desc_xml"].toString()));
        juce::PluginDescription desc;
        if (xml == nullptr || ! desc.loadFromXml (*xml))
        {
            error = role + ": bad plugin description";
            return false;
        }

        if (note != nullptr)
            note ("loading " + role + " " + desc.name);
        juce::String err;
        auto plugin = formats.createPluginInstance (desc, spec.sampleRate, spec.block, err);
        if (plugin == nullptr)
        {
            error = role + ": load failed: " + err;
            return false;
        }

        const auto statePath = st["state"].toString();
        if (statePath.isNotEmpty())
        {
            juce::MemoryBlock state;
            if (! juce::File (statePath).loadFileAsData (state))
            {
                error = "cannot read " + role + " state";
                return false;
            }
            plugin->setStateInformation (state.getData(), (int) state.getSize());
            // same JUCE-VST3 wrapper trap as the offline clones: a bypass PARAMETER may travel in the state
            if (auto* bypass = plugin->getBypassParameter())
                bypass->setValueNotifyingHost (0.0f);
        }

        // parameters by name (normalized 0..1), applied after the state: lets a render be built from defaults
        // alone, to compare with the same settings arriving as a saved state
        if (auto* params = st["params"].getDynamicObject())
            for (const auto& kv : params->getProperties())
            {
                bool done = false;
                for (auto* prm : plugin->getParameters())
                    if (prm->getName (128) == kv.name.toString())
                    {
                        prm->setValue ((float) (double) kv.value);
                        done = true;
                        break;
                    }
                if (! done)
                {
                    error = role + ": no parameter named '" + kv.name.toString() + "'";
                    return false;
                }
            }
        out = { role, std::move (plugin) };
        return true;
    }

    inline bool buildSpec (const juce::var& job, juce::AudioPluginFormatManager& formats,
                           RigRender::Spec& spec, juce::String& error,
                           const std::function<void (const juce::String&)>& note)
    {
        auto num = [&job] (const char* k, double d) { return job.hasProperty (k) ? (double) job[k] : d; };

        spec.sampleRate  = num ("rate", 48000.0);
        spec.block       = juce::jlimit (32, 8192, (int) num ("block", 512));
        spec.tailSeconds = num ("tail", 2.0);
        spec.compensate  = job.hasProperty ("compensate") ? (bool) job["compensate"] : true;
        spec.dryParallel = job.hasProperty ("dry_parallel") ? (bool) job["dry_parallel"] : false;
        spec.settleMs    = (int) num ("settle", 500.0);
        spec.out         = juce::File (job["out"].toString());

        auto* tracks = job["tracks"].getArray();
        if (tracks == nullptr || tracks->isEmpty())
        {
            error = "job has no tracks";
            return false;
        }

        for (const auto& tj : *tracks)
        {
            RigRender::Track tr;
            const auto source = tj["source"].toString();
            auto tnum = [&tj] (const char* k, double d) { return tj.hasProperty (k) ? (double) tj[k] : d; };

            if (source == "impulse")
            {
                tr.impulseAt = (juce::int64) tnum ("impulse_at", 1000.0);
                tr.stages.push_back ({ "source", std::make_unique<RigRender::ImpulseSource> (tr.impulseAt, (float) tnum ("impulse_amp", 0.1)) });
            }
            else if (source == "file")
            {
                juce::WavAudioFormat wav;
                const juce::File f (tj["source_path"].toString());
                std::unique_ptr<juce::AudioFormatReader> reader (f.existsAsFile() ? wav.createReaderFor (f.createInputStream().release(), true) : nullptr);
                if (reader == nullptr) { error = "cannot read source wav " + f.getFullPathName(); return false; }
                if (std::abs (reader->sampleRate - spec.sampleRate) > 0.5)
                {
                    error = "source wav is " + juce::String (reader->sampleRate) + " Hz, the job runs at " + juce::String (spec.sampleRate);
                    return false;
                }
                auto data = std::make_shared<juce::AudioBuffer<float>> (2, (int) reader->lengthInSamples);
                reader->read (data.get(), 0, (int) reader->lengthInSamples, 0, true, true);
                if (reader->numChannels == 1)
                    data->copyFrom (1, 0, *data, 0, 0, data->getNumSamples());
                tr.sourceSamples = data->getNumSamples();
                tr.stages.push_back ({ "source", std::make_unique<RigRender::FileSource> (data) });
            }
            else
            {
                tr.sequence = sequenceFromEvents (tj["events"], error);
                if (error.isNotEmpty())
                    return false;
            }

            if (auto* stages = tj["stages"].getArray())
                for (const auto& st : *stages)
                {
                    RigRender::Stage stage;
                    if (! buildStage (st, formats, spec, stage, error, note))
                        return false;
                    tr.stages.push_back (std::move (stage));
                }
            spec.tracks.push_back (std::move (tr));
        }

        if (auto* master = job["master"].getArray())
            for (const auto& st : *master)
            {
                RigRender::Stage stage;
                if (! buildStage (st, formats, spec, stage, error, note))
                    return false;
                spec.master.push_back (std::move (stage));
            }
        return true;
    }
}
