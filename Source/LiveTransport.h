#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "SoundDiag.h"

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

    // Sound diagnostics (SoundDiag.h): the player reports what the device actually got.
    AudioProbe probe;

    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        transport.setRate (d->getCurrentSampleRate());
        rate.store (d->getCurrentSampleRate());
        probe.starts.fetch_add (1);
        juce::AudioProcessorPlayer::audioDeviceAboutToStart (d);
    }

    void audioDeviceStopped() override
    {
        probe.stops.fetch_add (1);
        juce::AudioProcessorPlayer::audioDeviceStopped();
    }

    void audioDeviceError (const juce::String& message) override
    {
        probe.onError (message);
        juce::AudioProcessorPlayer::audioDeviceError (message);
    }

    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m) override
    {
        probe.midiHw.fetch_add (1);
        if (m.isNoteOn()) probe.midiHwNotes.fetch_add (1);
        juce::AudioProcessorPlayer::handleIncomingMidiMessage (source, m);
    }

    void audioDeviceIOCallbackWithContext (const float* const* in, int nIn, float* const* out, int nOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override
    {
        const auto t0 = juce::Time::getHighResolutionTicks();
        juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext (in, nIn, out, nOut, numSamples, ctx);
        transport.advance (numSamples);
        probe.onBlock (out, nOut, numSamples,
                       juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0), rate.load());
    }

private:
    LiveTransport& transport;
    std::atomic<double> rate { 48000.0 };
};
