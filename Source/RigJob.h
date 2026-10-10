#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "RigRender.h"
#include "EventSequence.h"
#include "MixStrips.h"

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
    // A track whose source is an ARA document: the plug-in (stage 0 of the track) is bound to a document made from a
    // clip of the file, and the render runs it as a playback renderer. The bench's worker sets the document up.
    struct AraPlan
    {
        juce::AudioProcessor* plugin = nullptr;
        std::shared_ptr<juce::AudioBuffer<float>> data;
        double fileSampleRate = 48000.0;
        double start = 0.0, offset = 0.0, length = 0.0;     // seconds
    };

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
        if (kind == "strip")
        {
            // the same channel strip the live bench has: gain (dB), balance (-1..1), mute
            auto strip = std::make_unique<TrackStrip>();
            strip->setGainDb ((float) (double) st.getProperty ("gain_db", 0.0));
            strip->setBalance ((float) (double) st.getProperty ("balance", 0.0));
            strip->setMuted ((bool) st.getProperty ("mute", false));
            out = { role, std::move (strip) };
            return true;
        }
        if (kind == "tail")
        {
            const double t60 = (double) st["tail_t60"];
            out = { role, std::make_unique<RigRender::KnownTail> (t60, st.hasProperty ("tail_declared") ? (double) st["tail_declared"] : t60) };
            return true;
        }

        if (kind == "probe")
        {
            const auto which = st["probe"].toString();
            if (which == "playhead") { out = { role, std::make_unique<RigRender::PlayheadProbe>() }; return true; }
            if (which == "gain")     { out = { role, std::make_unique<RigRender::GainProbe>() };     return true; }
            error = role + ": unknown probe '" + which + "'";
            return false;
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
                           const std::function<void (const juce::String&)>& note,
                           std::vector<AraPlan>* araPlans = nullptr)
    {
        auto num = [&job] (const char* k, double d) { return job.hasProperty (k) ? (double) job[k] : d; };

        spec.sampleRate  = num ("rate", 48000.0);
        spec.block       = juce::jlimit (32, 8192, (int) num ("block", 512));
        spec.tailSeconds = num ("tail", 2.0);
        spec.compensate  = job.hasProperty ("compensate") ? (bool) job["compensate"] : true;
        spec.dryParallel = job.hasProperty ("dry_parallel") ? (bool) job["dry_parallel"] : false;
        spec.settleMs    = (int) num ("settle", 500.0);
        spec.bpm         = num ("bpm", 120.0);
        spec.automationBlockQuantised = job.hasProperty ("automation_quantised") ? (bool) job["automation_quantised"] : false;
        if (auto* sig = job["time_sig"].getArray())
            if (sig->size() == 2) { spec.sigNum = (int) (*sig)[0]; spec.sigDen = (int) (*sig)[1]; }
        if (auto* pat = job["block_pattern"].getArray())
            for (const auto& v : *pat)
                spec.blockPattern.push_back ((int) v);
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
            AraPlan pendingAra;
            const auto source = tj["source"].toString();
            auto tnum = [&tj] (const char* k, double d) { return tj.hasProperty (k) ? (double) tj[k] : d; };

            if (source == "impulse")
            {
                tr.impulseAt = (juce::int64) tnum ("impulse_at", 1000.0);
                tr.stages.push_back ({ "source", std::make_unique<RigRender::ImpulseSource> (tr.impulseAt, (float) tnum ("impulse_amp", 0.1)) });
            }
            else if (source == "file" || source == "ara")
            {
                juce::WavAudioFormat wav;
                const juce::File f (tj["source_path"].toString());
                std::unique_ptr<juce::AudioFormatReader> reader (f.existsAsFile() ? wav.createReaderFor (f.createInputStream().release(), true) : nullptr);
                if (reader == nullptr) { error = "cannot read source wav " + f.getFullPathName(); return false; }
                // a plain clip is played sample for sample, so its rate must be the render's; an ARA plug-in converts it
                if (source == "file" && std::abs (reader->sampleRate - spec.sampleRate) > 0.5)
                {
                    error = "source wav is " + juce::String (reader->sampleRate) + " Hz, the job runs at " + juce::String (spec.sampleRate);
                    return false;
                }
                auto data = std::make_shared<juce::AudioBuffer<float>> (2, (int) reader->lengthInSamples);
                reader->read (data.get(), 0, (int) reader->lengthInSamples, 0, true, true);
                if (reader->numChannels == 1)
                    data->copyFrom (1, 0, *data, 0, 0, data->getNumSamples());
                // the clip: where it sits on the timeline, where in the file it starts, how long, how loud (seconds / dB)
                const auto toSamples = [&] (const char* key, double fallback) { return (juce::int64) std::llround (tnum (key, fallback) * spec.sampleRate); };
                const juce::int64 clipStart  = toSamples ("clip_start", 0.0);
                const juce::int64 clipOffset = juce::jlimit<juce::int64> (0, data->getNumSamples(), toSamples ("clip_offset", 0.0));
                const juce::int64 clipLength = tj.hasProperty ("clip_length") ? toSamples ("clip_length", 0.0) : -1;
                const float gain = (float) juce::Decibels::decibelsToGain (tnum ("clip_gain_db", 0.0));
                if (source == "ara")
                {
                    // the plug-in reads the file itself, through the ARA document: no source stage, the plug-in is stage 0
                    const double fileRate = reader->sampleRate;
                    const double startSec = tnum ("clip_start", 0.0), offsetSec = tnum ("clip_offset", 0.0);
                    const double inFileSec = (double) data->getNumSamples() / fileRate - offsetSec;
                    const double lenSec = tj.hasProperty ("clip_length") ? juce::jmin (tnum ("clip_length", 0.0), inFileSec) : inFileSec;
                    tr.sourceSamples = (juce::int64) std::llround ((startSec + lenSec) * spec.sampleRate);
                    pendingAra = { nullptr, data, fileRate, startSec, offsetSec, lenSec };
                    if (gain != 1.0f)
                    {
                        error = "ara: clip_gain_db is not applied by an ARA playback region";
                        return false;
                    }
                }
                else
                {
                    auto clip = std::make_unique<RigRender::FileSource> (data, clipStart, clipOffset, clipLength, gain);
                    tr.sourceSamples = clip->endOnTimeline();
                    tr.stages.push_back ({ "source", std::move (clip) });
                }
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
            if (source == "ara")
            {
                if (tr.stages.empty() || araPlans == nullptr)
                {
                    error = "ara: the track needs the ARA plug-in as its first stage";
                    return false;
                }
                pendingAra.plugin = tr.stages[0].plugin.get();
                araPlans->push_back (pendingAra);
            }
            spec.tracks.push_back (std::move (tr));
        }

        // automation: [{ track: <index> | "master", role, param: <name>, points: [[seconds, value 0..1], ...] }]
        if (auto* list = job["automation"].getArray())
            for (const auto& a : *list)
            {
                std::vector<RigRender::Stage>* stages = nullptr;
                if (a["track"].toString() == "master")
                    stages = &spec.master;
                else if ((int) a["track"] >= 0 && (int) a["track"] < (int) spec.tracks.size())
                    stages = &spec.tracks[(size_t) (int) a["track"]].stages;
                if (stages == nullptr) { error = "automation: no such track"; return false; }

                juce::AudioProcessor* target = nullptr;
                for (auto& st : *stages)
                    if (st.role == a["role"].toString() && st.plugin != nullptr) { target = st.plugin.get(); break; }
                if (target == nullptr) { error = "automation: no stage with role '" + a["role"].toString() + "'"; return false; }

                RigRender::Spec::Automation au;
                for (auto* prm : target->getParameters())
                    if (prm->getName (128) == a["param"].toString()) { au.param = prm; break; }
                if (au.param == nullptr) { error = "automation: no parameter named '" + a["param"].toString() + "'"; return false; }
                if (auto* pts = a["points"].getArray())
                    for (const auto& pt : *pts)
                        au.points.push_back ({ (juce::int64) std::llround ((double) (*pt.getArray())[0] * spec.sampleRate),
                                               (float) (double) (*pt.getArray())[1] });
                spec.automation.push_back (std::move (au));
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
