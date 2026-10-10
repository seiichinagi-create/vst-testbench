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
    bool  isAudible() const         { return ! muted.load() && ! soloSilenced.load(); }
    // Solo: the mixer sets `soloSilenced` on every track that is not soloed while any track is (MainComponent::updateSolo).
    void  setSolo (bool b)          { solo.store (b); }
    bool  isSolo() const            { return solo.load(); }
    void  setSoloSilenced (bool b)  { soloSilenced.store (b); }

    // Peak since the previous read, per side (the meter's source).
    float takePeakL() { return peakL.exchange (0.0f); }
    float takePeakR() { return peakR.exchange (0.0f); }

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
        gains (gainDb.load(), balance.load(), muted.load() || soloSilenced.load(), tl, tr);
        if (first) { lastL = tl; lastR = tr; first = false; }

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
        }
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
    std::atomic<float> gainDb { 0.0f }, balance { 0.0f }, peakL { 0.0f }, peakR { 0.0f };
    std::atomic<bool> muted { false }, solo { false }, soloSilenced { false };
    float lastL = 1.0f, lastR = 1.0f;   // audio thread only
    bool first = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackStrip)
};

//==============================================================================
// Passes only the MIDI channels of a mask (bit 0 = channel 1). Channel-less messages (sysex, clock) pass. The default
// mask is every channel: the node then changes nothing. One per VSTi track (docs/TRACKS.md, section on MIDI routing).
class MidiChannelFilter : public juce::AudioProcessor
{
public:
    MidiChannelFilter() : AudioProcessor (BusesProperties()) {}

    const juce::String getName() const override { return "MIDI filter"; }

    void setMask (juce::uint32 m) { mask.store (m & 0xFFFFu); }
    juce::uint32 getMask() const { return mask.load(); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midi) override
    {
        const auto m = mask.load();
        if (m == 0xFFFFu)
            return;
        juce::MidiBuffer kept;
        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            const int ch = msg.getChannel();                 // 1..16, 0 for channel-less messages
            if (ch == 0 || (m & (1u << (ch - 1))) != 0)
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiChannelFilter)
};
