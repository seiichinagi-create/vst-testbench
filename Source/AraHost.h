#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <memory>

#if JUCE_PLUGINHOST_ARA

//==============================================================================
// The bench as an ARA host (Celemony's Audio Random Access): the plug-in is handed a DOCUMENT instead of a stream. The
// document is the smallest one that means something: one musical context, one region sequence, one audio source (the
// clip's file), one audio modification, one playback region (the clip's place on the timeline). The plug-in is bound to
// it as a PLAYBACK RENDERER, and then renders the region when the rig runs it, like any other source.
//
// Everything happens on the message thread; the factory comes back asynchronously. Keep the session alive for as long as
// the plug-in is used, and let it go BEFORE the plug-in instance is destroyed is not possible here (the graph destroys
// the instance on the message thread when the render ends), so the session releases its model objects first and the
// bound extension last, in the order JUCE's own notes ask for.
//==============================================================================
class AraSession
{
public:
    struct Clip
    {
        std::shared_ptr<juce::AudioBuffer<float>> data;   // the whole file
        double sampleRate = 48000.0;
        juce::int64 start = 0;      // where the region sits on the timeline, in samples
        juce::int64 offset = 0;     // where in the file it starts
        juce::int64 length = 0;     // how long it is
    };

    ~AraSession()
    {
        // regions first (they deregister from the renderer), then the renderer, the model, the document
        renderer.reset();
        region.reset();
        modification.reset();
        source.reset();
        sequence.reset();
        musical.reset();
        extension = {};
        doc.reset();
    }

    juce::String getBoundWith() const { return boundWith; }

    // done(ok, message) is called on the message thread.
    void start (juce::AudioPluginInstance& plugin, Clip clip, std::function<void (bool, juce::String)> done)
    {
        juce::createARAFactoryAsync (plugin, [this, &plugin, clip, done] (juce::ARAFactoryWrapper wrapper)
        {
            if (wrapper.get() == nullptr)
            {
                done (false, "the plug-in offers no ARA factory");
                return;
            }
            factory = wrapper;

            doc = juce::ARAHostDocumentController::create (factory, "VST TestBench rig",
                                                           std::make_unique<AudioAccess> (clip.data),
                                                           std::make_unique<Archiving>());
            if (doc == nullptr)
            {
                done (false, "could not create the ARA document controller");
                return;
            }

            auto& dc = doc->getDocumentController();
            const double sr = clip.sampleRate;
            {
                const juce::ARAEditGuard guard (dc);

                auto mcProps = juce::ARAHostModel::MusicalContext::getEmptyProperties();
                mcProps.name = "musical context";
                musical = std::make_unique<juce::ARAHostModel::MusicalContext> (hostRef<ARA::ARAMusicalContextHostRef> (1), dc, mcProps);

                auto rsProps = juce::ARAHostModel::RegionSequence::getEmptyProperties();
                rsProps.name = "track";
                rsProps.orderIndex = 0;
                rsProps.musicalContextRef = musical->getPluginRef();
                sequence = std::make_unique<juce::ARAHostModel::RegionSequence> (hostRef<ARA::ARARegionSequenceHostRef> (2), dc, rsProps);

                auto asProps = juce::ARAHostModel::AudioSource::getEmptyProperties();
                asProps.name = "audio file";
                asProps.persistentID = "rig-source-0";
                asProps.sampleCount = (ARA::ARASampleCount) clip.data->getNumSamples();
                asProps.sampleRate = sr;
                asProps.channelCount = (ARA::ARAChannelCount) clip.data->getNumChannels();
                asProps.merits64BitSamples = ARA::kARAFalse;
                source = std::make_unique<juce::ARAHostModel::AudioSource> (hostRef<ARA::ARAAudioSourceHostRef> (3), dc, asProps);
                source->enableAudioSourceSamplesAccess (true);

                auto amProps = juce::ARAHostModel::AudioModification::getEmptyProperties();
                amProps.name = "unmodified";
                amProps.persistentID = "rig-modification-0";
                modification = std::make_unique<juce::ARAHostModel::AudioModification> (hostRef<ARA::ARAAudioModificationHostRef> (4), dc, *source, amProps);

                auto prProps = juce::ARAHostModel::PlaybackRegion::getEmptyProperties();
                prProps.transformationFlags = ARA::kARAPlaybackTransformationNoChanges;
                prProps.startInModificationTime = (double) clip.offset / sr;
                prProps.durationInModificationTime = (double) clip.length / sr;
                prProps.startInPlaybackTime = (double) clip.start / sr;
                prProps.durationInPlaybackTime = (double) clip.length / sr;
                prProps.regionSequenceRef = sequence->getPluginRef();
                prProps.name = "clip";
                region = std::make_unique<juce::ARAHostModel::PlaybackRegion> (hostRef<ARA::ARAPlaybackRegionHostRef> (5), dc, *modification, prProps);
            }

            // Which roles a plug-in accepts is its own business (an editor such as Melodyne may want to be bound with the
            // editor roles too). Ask for the playback renderer alone first, then with more, and say which one worked.
            using Role = ARA::ARAPlugInInstanceRoleFlags;
            const Role P = ARA::kARAPlaybackRendererRole, E = ARA::kARAEditorRendererRole, V = ARA::kARAEditorViewRole;
            struct Try { Role known, assigned; const char* name; };
            const Try tries[] = { { P, P, "known P, assigned P" },
                                  { (Role) (P | E | V), P, "known P+E+V, assigned P" },
                                  { (Role) (P | E | V), (Role) (P | E), "known P+E+V, assigned P+E" },
                                  { (Role) (P | E | V), (Role) (P | E | V), "known P+E+V, assigned P+E+V" } };
            juce::String tried;
            for (const auto& t : tries)
            {
                extension = doc->bindDocumentToPluginInstance (plugin, t.known, t.assigned);
                tried += juce::String (t.name) + (extension.isValid() ? ": bound; " : ": refused; ");
                if (extension.isValid())
                {
                    boundWith = t.name;
                    break;
                }
            }
            if (! extension.isValid())
            {
                done (false, "the plug-in refused to be bound to the ARA document (" + tried + ")");
                return;
            }
            renderer = std::make_unique<juce::ARAHostModel::PlaybackRendererInterface> (extension.getPlaybackRendererInterface());
            if (! renderer->isValid())
            {
                done (false, "the plug-in did not provide a playback renderer");
                return;
            }
            renderer->add (*region);            // before prepareToPlay, as JUCE requires
            done (true, {});
        });
    }

private:
    template <typename Ref>
    static Ref hostRef (std::uintptr_t id) { return reinterpret_cast<Ref> (id); }

    // The audio the plug-in reads: samples out of the clip's file, zeros outside it.
    class AudioAccess : public ARA::Host::AudioAccessControllerInterface
    {
    public:
        explicit AudioAccess (std::shared_ptr<juce::AudioBuffer<float>> d) : data (std::move (d)) {}

        ARA::ARAAudioReaderHostRef createAudioReaderForSource (ARA::ARAAudioSourceHostRef, bool use64BitSamples) noexcept override
        {
            return reinterpret_cast<ARA::ARAAudioReaderHostRef> (new Reader { use64BitSamples });
        }

        bool readAudioSamples (ARA::ARAAudioReaderHostRef ref, ARA::ARASamplePosition position,
                               ARA::ARASampleCount count, void* const buffers[]) noexcept override
        {
            const auto* reader = reinterpret_cast<const Reader*> (ref);
            const juce::int64 total = data->getNumSamples();
            for (int c = 0; c < data->getNumChannels(); ++c)
                for (juce::int64 i = 0; i < count; ++i)
                {
                    const juce::int64 k = position + i;
                    const float v = (k >= 0 && k < total) ? data->getSample (c, (int) k) : 0.0f;
                    if (reader->use64)
                        static_cast<double*> (buffers[c])[i] = (double) v;
                    else
                        static_cast<float*> (buffers[c])[i] = v;
                }
            return true;
        }

        void destroyAudioReader (ARA::ARAAudioReaderHostRef ref) noexcept override
        {
            delete reinterpret_cast<Reader*> (ref);
        }

    private:
        struct Reader { bool use64; };
        std::shared_ptr<juce::AudioBuffer<float>> data;
    };

    // There is no saved document to restore and none to save: an empty archive.
    class Archiving : public ARA::Host::ArchivingControllerInterface
    {
    public:
        ARA::ARASize getArchiveSize (ARA::ARAArchiveReaderHostRef) noexcept override { return 0; }
        bool readBytesFromArchive (ARA::ARAArchiveReaderHostRef, ARA::ARASize, ARA::ARASize, ARA::ARAByte[]) noexcept override { return false; }
        bool writeBytesToArchive (ARA::ARAArchiveWriterHostRef, ARA::ARASize, ARA::ARASize, const ARA::ARAByte[]) noexcept override { return false; }
        void notifyDocumentArchivingProgress (float) noexcept override {}
        void notifyDocumentUnarchivingProgress (float) noexcept override {}
        ARA::ARAPersistentID getDocumentArchiveID (ARA::ARAArchiveReaderHostRef) noexcept override { return nullptr; }
    };

    juce::String boundWith;
    juce::ARAFactoryWrapper factory;
    std::unique_ptr<juce::ARAHostDocumentController> doc;
    std::unique_ptr<juce::ARAHostModel::MusicalContext> musical;
    std::unique_ptr<juce::ARAHostModel::RegionSequence> sequence;
    std::unique_ptr<juce::ARAHostModel::AudioSource> source;
    std::unique_ptr<juce::ARAHostModel::AudioModification> modification;
    std::unique_ptr<juce::ARAHostModel::PlaybackRegion> region;
    juce::ARAHostModel::PlugInExtensionInstance extension;
    std::unique_ptr<juce::ARAHostModel::PlaybackRendererInterface> renderer;
};

#endif   // JUCE_PLUGINHOST_ARA
