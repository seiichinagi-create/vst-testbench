#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <functional>

//==============================================================================
// Phase C: a MIDI file's future is known, so a VSTi can be bounced offline
// (faster than realtime) into a wav that then rides the existing
// file -> GPU FX -> VST FX -> PRE-RENDER chain unchanged. Live MIDI input
// stays out of scope: its future is unknown, so it keeps the ASIO direct path.
//
// The caller passes an OFFLINE clone of the instrument (state-synced from the
// live node, RenderAhead style) that nobody else touches while the thread runs.
//==============================================================================
class MidiBounceEngine : private juce::Thread
{
public:
    MidiBounceEngine() : juce::Thread ("midi-bounce") {}
    ~MidiBounceEngine() override { stopBounce(); }

    // Fires on the message thread. info carries the speed text on success,
    // the reason on failure.
    std::function<void (bool ok, juce::File outFile, juce::String info)> onDone;

    // sequence: all tracks merged, timestamps already in SECONDS.
    void startBounce (juce::MidiMessageSequence sequence, juce::AudioPluginInstance* instrument,
                      double rate, const juce::File& outFile)
    {
        stopBounce();
        seq        = std::move (sequence);
        inst       = instrument;
        sampleRate = rate > 0 ? rate : 48000.0;
        out        = outFile;
        progress = 0.0f; speedX = 0.0f;
        startThread (juce::Thread::Priority::low);   // same rule as render-ahead: never starve audio
    }

    void stopBounce()        { stopThread (5000); }
    bool isBouncing() const  { return isThreadRunning(); }

    std::atomic<float> progress { 0.0f };   // 0..1
    std::atomic<float> speedX   { 0.0f };   // bounced seconds / elapsed seconds

private:
    static constexpr double tailSeconds = 4.0;   // reverb/release ring-out after the last event

    void finish (bool ok, const juce::String& info)
    {
        if (threadShouldExit())
            return;   // owner is tearing down; no callback
        auto cb = onDone;
        auto f  = out;
        juce::MessageManager::callAsync ([cb, ok, f, info]
        {
            if (cb != nullptr)
                cb (ok, f, info);
        });
    }

    void run() override
    {
        const double endTime = seq.getNumEvents() > 0 ? seq.getEndTime() : 0.0;
        const juce::int64 total = (juce::int64) ((endTime + tailSeconds) * sampleRate);

        if (seq.getNumEvents() == 0)                    { finish (false, "no events in MIDI file"); return; }
        // 150M samples: the same ceiling the PRE-RENDER cache uses (~52 min).
        if (total > 150'000'000)                        { finish (false, "MIDI file too long to bounce"); return; }
        if (inst == nullptr)                            { finish (false, "no instrument clone"); return; }

        const int block = 4096;
        inst->setNonRealtime (true);
        inst->prepareToPlay (sampleRate, block);
        const int chans = juce::jmax (2, inst->getTotalNumOutputChannels());
        juce::AudioBuffer<float> buf (chans, block);

        out.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (out.createOutputStream());
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (stream != nullptr)
            writer = wavFormat.createWriterFor (stream,   // takes the stream on success
                         juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
        {
            inst->releaseResources();
            finish (false, "cannot write " + out.getFileName());
            return;
        }

        juce::MidiBuffer midi;
        int evIndex = 0;
        juce::int64 pos = 0;
        bool ok = true;
        const double t0 = juce::Time::getMillisecondCounterHiRes();

        while (pos < total && ! threadShouldExit())
        {
            const int n = (int) juce::jmin ((juce::int64) block, total - pos);

            midi.clear();
            const double blockEnd = (double) (pos + n) / sampleRate;
            while (evIndex < seq.getNumEvents())
            {
                const auto* ev = seq.getEventPointer (evIndex);
                const double t = ev->message.getTimeStamp();
                if (t >= blockEnd)
                    break;
                const int offset = juce::jlimit (0, n - 1, (int) ((juce::int64) (t * sampleRate) - pos));
                midi.addEvent (ev->message, offset);
                ++evIndex;
            }

            buf.clear();
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), chans, 0, n);
            inst->processBlock (view, midi);

            const float* outChans[2] = { buf.getReadPointer (0),
                                         buf.getReadPointer (juce::jmin (1, chans - 1)) };
            if (! writer->writeFromFloatArrays (outChans, 2, n))
            {
                ok = false;
                break;
            }
            pos += n;

            progress.store (total > 0 ? (float) pos / (float) total : 0.0f);
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            if (elapsed > 0.01)
                speedX.store ((float) ((double) pos / sampleRate / elapsed));
        }

        writer.reset();   // flush + close before anyone reads the file
        inst->releaseResources();

        if (threadShouldExit())
            return;
        if (! ok)
        {
            finish (false, "disk write failed");
            return;
        }
        finish (true, juce::String::formatted ("%.1fx realtime", speedX.load()));
    }

    juce::MidiMessageSequence seq;
    juce::AudioPluginInstance* inst = nullptr;
    double sampleRate = 48000.0;
    juce::File out;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiBounceEngine)
};
