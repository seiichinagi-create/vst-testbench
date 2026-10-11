#pragma once
#include <JuceHeader.h>
#include <atomic>

// What the live host tells a plug-in about the transport (tempo, position, playing). The rig has its own
// (RigRender::RigPlayHead); this one is advanced by the live audio callback, one block at a time.
// Position is the start of the block being processed, so a plug-in that reads it inside processBlock sees
// sample-exact time. Without it a plug-in that plays its own project on the transport (Synthesizer V) stays silent.
class LiveTransport : public juce::AudioPlayHead
{
public:
    void start (double fromSeconds = 0.0)
    {
        pos.store ((juce::int64) (fromSeconds * sampleRate.load()));
        playing.store (true);
    }
    void stop (bool rewind = true)
    {
        playing.store (false);
        if (rewind) pos.store (0);
    }
    void seek (double seconds) { pos.store ((juce::int64) (seconds * sampleRate.load())); }
    void setTempo (double b) { bpm.store (juce::jlimit (10.0, 999.0, b)); }
    void setRate (double sr) { if (sr > 0.0) sampleRate.store (sr); }

    // Called by the audio callback after each block.
    void advance (int numSamples) { if (playing.load()) pos.fetch_add (numSamples); }

    bool isPlaying() const { return playing.load(); }
    double seconds() const { return (double) pos.load() / sampleRate.load(); }
    double tempo() const { return bpm.load(); }

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto p = pos.load();
        const double sr = sampleRate.load(), b = bpm.load();
        PositionInfo i;
        i.setBpm (b);
        i.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        i.setTimeInSamples (p);
        i.setTimeInSeconds ((double) p / sr);
        i.setPpqPosition ((double) p / sr * b / 60.0);
        i.setIsPlaying (playing.load());
        i.setIsRecording (false);
        i.setIsLooping (false);
        return i;
    }

private:
    std::atomic<juce::int64> pos { 0 };
    std::atomic<bool> playing { false };
    std::atomic<double> bpm { 120.0 }, sampleRate { 48000.0 };
};

// AudioProcessorPlayer that moves the transport after every block.
class TransportPlayer : public juce::AudioProcessorPlayer
{
public:
    explicit TransportPlayer (LiveTransport& t) : transport (t) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        transport.setRate (d->getCurrentSampleRate());
        juce::AudioProcessorPlayer::audioDeviceAboutToStart (d);
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int nIn, float* const* out, int nOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override
    {
        juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext (in, nIn, out, nOut, numSamples, ctx);
        transport.advance (numSamples);
    }

private:
    LiveTransport& transport;
};
