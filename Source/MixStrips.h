#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

// Channel strip: gain, balance, mute and a peak meter on one stereo pair. docs/TRACKS.md.
//
// At its defaults (0 dB, centre, not muted) it does NOTHING to the audio: not even a multiplication by 1.0, so a
// bench with one sounding track is bit-identical to a bench without strips. Changes ramp over the block (no zipper noise).
// Balance is a "balance" law, not a pan law: 0 = centre = unity on both sides; moving off centre only turns the far side down
// (a stereo track must not get quieter just because it sits in the middle).
class TrackStrip : public juce::AudioProcessor
{
public:
    TrackStrip()
        : AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Strip"; }

    // Message thread (or any thread: atomics).
    void  setGainDb (float db)      { gainDb.store (juce::jlimit (-100.0f, 12.0f, db)); }
    void  setBalance (float b)      { balance.store (juce::jlimit (-1.0f, 1.0f, b)); }
    void  setMuted (bool m)         { muted.store (m); }
    float getGainDb() const         { return gainDb.load(); }
    float getBalance() const        { return balance.load(); }
    bool  isMuted() const           { return muted.load(); }
    // The user's mix: not muted by the M button, not silenced by another track's solo. What a render or a bounce takes.
    bool  isAudible() const         { return ! muted.load() && ! soloSilenced.load(); }
    // The source-mode preset (live / file: AUDIO; VSTi: the instruments) silences the other tracks in the live graph only.
    void  setModeSilenced (bool b)  { modeSilenced.store (b); }
    bool  isModeSilenced() const    { return modeSilenced.load(); }
    bool  isSounding() const        { return isAudible() && ! modeSilenced.load(); }
    // Solo: the mixer sets `soloSilenced` on every track that is not soloed while any track is (MainComponent::updateSolo).
    void  setSolo (bool b)          { solo.store (b); }
    bool  isSolo() const            { return solo.load(); }
    void  setSoloSilenced (bool b)  { soloSilenced.store (b); }

    // Peak since the previous read, per side (the meter's source).
    float takePeakL() { return peakL.exchange (0.0f); }
    float takePeakR() { return peakR.exchange (0.0f); }
    // The same for the sound diagnostics (SoundDiag.h), on both sides of the gain: "signal arrives but the strip silences it".
    float takeDiagIn()  { return diagIn.exchange (0.0f); }
    float takeDiagOut() { return diagOut.exchange (0.0f); }

    // What the strip does to the signal, as two linear gains (testable without a graph).
    static void gains (float gainDbValue, float balanceValue, bool mutedValue, float& left, float& right)
    {
        if (mutedValue) { left = right = 0.0f; return; }
        const float g = gainDbValue <= -100.0f ? 0.0f : std::pow (10.0f, gainDbValue / 20.0f);
        left  = g * (balanceValue > 0.0f ? 1.0f - balanceValue : 1.0f);
        right = g * (balanceValue < 0.0f ? 1.0f + balanceValue : 1.0f);
    }

    void prepareToPlay (double, int) override { lastL = lastR = 1.0f; first = true; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        const int chans = buffer.getNumChannels();
        float tl, tr;
        gains (gainDb.load(), balance.load(), muted.load() || soloSilenced.load() || modeSilenced.load(), tl, tr);
        if (first) { lastL = tl; lastR = tr; first = false; }

        {
            float pkIn = 0.0f;
            for (int ch = 0; ch < juce::jmin (2, chans); ++ch)
                pkIn = juce::jmax (pkIn, buffer.getMagnitude (ch, 0, n));
            raise (diagIn, pkIn);
        }

        const float targets[2] = { tl, tr };
        float* last[2] = { &lastL, &lastR };
        std::atomic<float>* peaks[2] = { &peakL, &peakR };
        for (int ch = 0; ch < juce::jmin (2, chans); ++ch)
        {
            float* d = buffer.getWritePointer (ch);
            const float from = *last[ch], to = targets[ch];
            if (from == 1.0f && to == 1.0f)
            {
                // untouched: unity stays unity
            }
            else if (from == to)
            {
                for (int i = 0; i < n; ++i) d[i] *= to;
            }
            else
            {
                const float step = (to - from) / (float) juce::jmax (1, n);
                float g = from;
                for (int i = 0; i < n; ++i) { g += step; d[i] *= g; }
            }
            *last[ch] = to;

            float pk = 0.0f;
            for (int i = 0; i < n; ++i) pk = juce::jmax (pk, std::abs (d[i]));
            float cur = peaks[ch]->load();
            while (pk > cur && ! peaks[ch]->compare_exchange_weak (cur, pk)) {}
            raise (diagOut, pk);
        }
    }

    static void raise (std::atomic<float>& a, float v)
    {
        float cur = a.load();
        while (v > cur && ! a.compare_exchange_weak (cur, v)) {}
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    std::atomic<float> gainDb { 0.0f }, balance { 0.0f }, peakL { 0.0f }, peakR { 0.0f }, diagIn { 0.0f }, diagOut { 0.0f };
    std::atomic<bool> muted { false }, solo { false }, soloSilenced { false }, modeSilenced { false };
    float lastL = 1.0f, lastR = 1.0f;   // audio thread only
    bool first = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackStrip)
};

//==============================================================================
// Passes only the MIDI channels of a mask (bit 0 = channel 1). Channel-less messages (sysex, clock) pass. The default
// mask is every channel: the node then changes nothing. One per VSTi track (docs/TRACKS.md, section on MIDI routing).
// Live feed (the live graph): the node ignores its MIDI input and plays its own queue instead. MainComponent::routeMidi
// fills it with what came from this track's MIDI input device (or every device), so each INST track has its own input.
class MidiChannelFilter : public juce::AudioProcessor
{
public:
    MidiChannelFilter() : AudioProcessor (BusesProperties()) {}

    const juce::String getName() const override { return "MIDI filter"; }

    void setMask (juce::uint32 m) { mask.store (m & 0xFFFFu); }
    juce::uint32 getMask() const { return mask.load(); }

    void setLiveFeed (bool b) { liveFeed.store (b); }
    // Any thread: a message for this track (timestamp = Time::getMillisecondCounterHiRes() * 0.001, like MidiInput's).
    void feed (const juce::MidiMessage& m) { if (fed.load()) queue.addMessageToQueue (m); }

    void prepareToPlay (double sampleRate, int block) override { queue.reset (sampleRate); blockSize = block; fed.store (true); }
    void releaseResources() override {}

    // Note-ons that reached this filter / that it let through (cumulative; the sound diagnostics read them).
    juce::uint32 getNotesSeen() const   { return notesSeen.load(); }
    juce::uint32 getNotesPassed() const { return notesPassed.load(); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        if (liveFeed.load())
        {
            midi.clear();
            queue.removeNextBlockOfMessages (midi, buffer.getNumSamples() > 0 ? buffer.getNumSamples() : juce::jmax (1, blockSize));
        }
        const auto m = mask.load();
        if (m == 0xFFFFu)
        {
            for (const auto meta : midi)
                if (meta.getMessage().isNoteOn()) { notesSeen.fetch_add (1); notesPassed.fetch_add (1); }
            return;
        }
        juce::MidiBuffer kept;
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            const int ch = msg.getChannel();                 // 1..16, 0 for channel-less messages
            const bool pass = ch == 0 || (m & (1u << (ch - 1))) != 0;
            if (msg.isNoteOn()) { notesSeen.fetch_add (1); if (pass) notesPassed.fetch_add (1); }
            if (pass)
                kept.addEvent (msg, meta.samplePosition);
        }
        midi.swapWith (kept);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    std::atomic<juce::uint32> mask { 0xFFFFu };
    std::atomic<juce::uint32> notesSeen { 0 }, notesPassed { 0 };
    std::atomic<bool> liveFeed { false }, fed { false };
    juce::MidiMessageCollector queue;
    int blockSize = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiChannelFilter)
};
