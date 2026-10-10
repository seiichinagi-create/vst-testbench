#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>

//==============================================================================
// Stereo pass-through node placed in front of the device output. It lets the
// control server (and therefore an AI) HEAR what the bench plays: running
// peak / RMS meters, and a "record output to wav" tap fed from the audio thread
// through a lock-free ThreadedWriter.
//==============================================================================
class OutputTap : public juce::AudioProcessor
{
public:
    OutputTap()
        : AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          writerThread ("output-tap-writer")
    {
        writerThread.startThread (juce::Thread::Priority::low);
    }

    ~OutputTap() override { stopRecording(); writerThread.stopThread (2000); }

    // Peak / RMS since the previous read, per the louder channel.
    float takePeak()
    {
        const float p = peakAcc.exchange (0.0f);
        return p;
    }
    float takeRms()
    {
        const double sq = sumSq.exchange (0.0);
        const juce::int64 n = count.exchange (0);
        return n > 0 ? (float) std::sqrt (sq / (double) n) : 0.0f;
    }

    // Message thread only.
    bool startRecording (const juce::File& file, double sampleRate, juce::String& error)
    {
        stopRecording();
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr) { error = "cannot open " + file.getFullPathName(); return false; }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer =
            wav.createWriterFor (stream,   // takes the stream on success
                                 juce::AudioFormatWriterOptions{}
                                     .withSampleRate (sampleRate)
                                     .withNumChannels (2)
                                     .withBitsPerSample (32)
                                     .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) { error = "cannot create wav writer"; return false; }

        auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 65536);
        const juce::SpinLock::ScopedLockType sl (lock);
        recorder = std::move (threaded);
        recordedSamples = 0;
        return true;
    }

    // Returns the number of recorded sample frames.
    juce::int64 stopRecording()
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> old;
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            old = std::move (recorder);
        }
        const auto n = recordedSamples.load();
        old.reset();   // flushes and closes the file
        return n;
    }

    bool isRecording() const { const juce::SpinLock::ScopedLockType sl (lock); return recorder != nullptr; }
    juce::int64 getRecordedSamples() const { return recordedSamples.load(); }

    //==========================================================================
    const juce::String getName() const override { return "OutputTap"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        const int chans = juce::jmin (2, buffer.getNumChannels());
        float peak = 0.0f;
        double sq = 0.0;
        for (int c = 0; c < chans; ++c)
        {
            const float* d = buffer.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float v = d[i];
                peak = juce::jmax (peak, std::abs (v));
                sq += (double) v * v;
            }
        }
        if (chans > 0 && n > 0)
        {
            float cur = peakAcc.load();
            while (peak > cur && ! peakAcc.compare_exchange_weak (cur, peak)) {}
            sumSq.store (sumSq.load() + sq / (double) chans);
            count.store (count.load() + n);
        }

        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (sl.isLocked() && recorder != nullptr && chans > 0)
        {
            const float* src[2] = { buffer.getReadPointer (0), buffer.getReadPointer (chans - 1) };
            if (recorder->write (src, n))
                recordedSamples.store (recordedSamples.load() + n);
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override    { return false; }
    int getNumPrograms() override      { return 1; }
    int getCurrentProgram() override   { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    // The audio thread is the only writer of the meter accumulators; the
    // message thread reads and zeroes them. A lost update on the exchange is
    // harmless for a meter.
    std::atomic<float>  peakAcc { 0.0f };
    std::atomic<double> sumSq { 0.0 };
    std::atomic<juce::int64> count { 0 };

    juce::TimeSliceThread writerThread;
    mutable juce::SpinLock lock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recorder;
    std::atomic<juce::int64> recordedSamples { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputTap)
};
