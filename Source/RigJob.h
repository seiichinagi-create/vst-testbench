#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "RigRender.h"
#include "EventSequence.h"

//==============================================================================
// Job description shared by the bench (which writes it) and the rig worker process (which runs it).
//
//   { rate, block, tail, compensate, dry_parallel, settle, out,
//     source: "impulse" | "midi", impulse_at, impulse_amp, events: [...],
//     stages: [ { role, kind: "plugin", desc_xml, state }       plugin by PluginDescription XML
//             | { role, kind: "delay", delay_actual, delay_declared } ] }
//
// The bench resolves names to descriptions (it owns the plugin cache); the worker only instantiates.
//==============================================================================
namespace rigjob
{
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

        if (job["source"].toString() == "impulse")
        {
            spec.impulseAt = (juce::int64) num ("impulse_at", 1000.0);
            spec.stages.push_back ({ "source", std::make_unique<RigRender::ImpulseSource> (spec.impulseAt, (float) num ("impulse_amp", 0.1)) });
        }
        else if (job["source"].toString() == "file")
        {
            juce::WavAudioFormat wav;
            const juce::File f (job["source_path"].toString());
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
            spec.sourceSamples = data->getNumSamples();
            spec.stages.push_back ({ "source", std::make_unique<RigRender::FileSource> (data) });
        }
        else
        {
            spec.sequence = sequenceFromEvents (job["events"], error);
            if (error.isNotEmpty())
                return false;
        }

        auto* stages = job["stages"].getArray();
        if (stages == nullptr)
        {
            error = "job has no stages";
            return false;
        }

        for (const auto& st : *stages)
        {
            const auto role = st["role"].toString();
            if (st["kind"].toString() == "delay")
            {
                const int actual = (int) st["delay_actual"];
                spec.stages.push_back ({ role, std::make_unique<RigRender::KnownDelay> (actual, st.hasProperty ("delay_declared") ? (int) st["delay_declared"] : actual) });
                continue;
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
            spec.stages.push_back ({ role, std::move (plugin) });
        }
        return true;
    }
}
