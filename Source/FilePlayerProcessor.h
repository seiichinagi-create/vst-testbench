#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

//==============================================================================
// Graph node that plays an audio file through an AudioTransportSource.
// The reader is created by the owner (MainComponent) so that format handling
// and the ffmpeg fallback stay in one place.
//==============================================================================
class FilePlayerProcessor : public juce::AudioProcessor
{
public:
    FilePlayerProcessor()
        : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        readThread.startThread();
    }

    ~FilePlayerProcessor() override
    {
        transport.setSource (nullptr);
        readerSource.reset();
    }

    // Takes ownership of the reader. Safe to call while the graph is running.
    bool loadReader (std::unique_ptr<juce::AudioFormatReader> reader, bool shouldLoop)
    {
        if (reader == nullptr)
            return false;

        transport.stop();
        transport.setSource (nullptr);
        external = nullptr;

        readerRate = reader->sampleRate;
        readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
        readerSource->setLooping (shouldLoop);
        transport.setSource (readerSource.get(), 65536, &readThread, readerRate);
        transport.setPosition (0.0);
        return true;
    }

    // Switch the transport to an external source (PRE-RENDER cache). The
    // reader source is kept alive so reattachReader() can restore live mode.
    void attachExternalSource (juce::PositionableAudioSource* src, double sourceRate)
    {
        const double keepPos = transport.getCurrentPosition();
        transport.stop();
        transport.setSource (nullptr);
        external = src;
        if (src != nullptr)
        {
            transport.setSource (src, 0, nullptr, sourceRate);
            transport.setPosition (keepPos);
        }
    }

    void reattachReader()
    {
        const double keepPos = transport.getCurrentPosition();
        transport.stop();
        transport.setSource (nullptr);
        external = nullptr;
        if (readerSource != nullptr)
        {
            transport.setSource (readerSource.get(), 65536, &readThread, readerRate);
            transport.setPosition (keepPos);
        }
    }

    bool isUsingExternalSource() const noexcept { return external != nullptr; }

    void setLooping (bool shouldLoop)
    {
        if (readerSource != nullptr)
            readerSource->setLooping (shouldLoop);
    }

    bool hasFile() const noexcept { return readerSource != nullptr; }

    juce::AudioTransportSource transport;

    //== AudioProcessor ==
    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        transport.prepareToPlay (samplesPerBlock, sampleRate);
    }

    void releaseResources() override { transport.releaseResources(); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples());
        transport.getNextAudioBlock (info);
    }

    const juce::String getName() const override            { return "File Player"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override    { return nullptr; }
    bool hasEditor() const override                        { return false; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override   {}

private:
    juce::TimeSliceThread readThread { "audio file playback" };
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::PositionableAudioSource* external = nullptr;   // not owned
    double readerRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilePlayerProcessor)
};
