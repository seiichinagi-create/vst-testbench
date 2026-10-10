#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "MixStrips.h"

// The mixer: AUDIO, INST 1-3 and MASTER side by side (docs/TRACKS.md). A strip shows the track's source / plug-in, the
// MIDI channels it listens to (INST), a mute button, a balance control, a gain fader and a level meter. It talks to the
// TrackStrip / MidiChannelFilter processors directly (atomics); loading, the editor and removal go through hooks.
class MixerPanel : public juce::Component, private juce::Timer
{
public:
    enum { numTracks = 5, masterIndex = 4 };   // 0 AUDIO, 1..3 INST 1-3, 4 MASTER

    struct Hooks
    {
        std::function<void (int)> load, editor, remove;      // track 1..3 (INST)
        std::function<juce::String (int)> describe;          // what the track plays: a plug-in name, a file, the live input
        std::function<void()> soloChanged;                   // a solo button was pressed: the host recomputes who is silenced
    };

    MixerPanel (TrackStrip* const (&strips)[4], TrackStrip* master, MidiChannelFilter* const (&filters)[4], Hooks h)
        : hooks (std::move (h))
    {
        for (int i = 0; i < numTracks; ++i)
        {
            views[i] = std::make_unique<StripView> (*this, i, i == masterIndex ? master : strips[i], i >= 1 && i <= 3 ? filters[i] : nullptr);
            addAndMakeVisible (*views[i]);
        }
        startTimerHz (30);
    }

    // "all", "none", or "1,3,5-8" -> a channel mask (bit 0 = channel 1)
    static juce::uint32 parseChannels (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        if (t == "all") return 0xFFFFu;
        juce::uint32 m = 0;
        for (auto& tok : juce::StringArray::fromTokens (t, ", ", ""))
        {
            const int dash = tok.indexOfChar ('-');
            const int a = (dash > 0 ? tok.substring (0, dash) : tok).getIntValue();
            const int b = dash > 0 ? tok.substring (dash + 1).getIntValue() : a;
            for (int c = juce::jmax (1, a); c <= juce::jmin (16, b); ++c)
                m |= 1u << (c - 1);
        }
        return m;
    }

    static juce::String formatChannels (juce::uint32 m)
    {
        if ((m & 0xFFFFu) == 0xFFFFu) return "all";
        if ((m & 0xFFFFu) == 0) return "none";
        juce::String s;
        for (int c = 1; c <= 16;)
        {
            if ((m & (1u << (c - 1))) == 0) { ++c; continue; }
            int e = c;
            while (e < 16 && (m & (1u << e)) != 0) ++e;   // e = last channel of the run
            if (s.isNotEmpty()) s << ",";
            s << (e > c ? juce::String (c) + "-" + juce::String (e) : juce::String (c));
            c = e + 1;
        }
        return s;
    }

    void refresh() { for (auto& v : views) v->refresh(); }

    void resized() override
    {
        auto r = getLocalBounds();
        const int w = r.getWidth() / numTracks;
        for (int i = 0; i < numTracks; ++i)
            views[i]->setBounds (i == numTracks - 1 ? r : r.removeFromLeft (w).reduced (3, 0));
    }

    Hooks hooks;

private:
    void timerCallback() override
    {
        for (auto& v : views) v->tick();
        if (++slow >= 15) { slow = 0; refresh(); }
    }

    //==========================================================================
    class StripView : public juce::Component
    {
    public:
        StripView (MixerPanel& o, int idx, TrackStrip* s, MidiChannelFilter* f)
            : owner (o), index (idx), strip (s), filter (f)
        {
            static const char* names[] = { "AUDIO", "INST 1", "INST 2", "INST 3", "MASTER" };
            title.setText (names[idx], juce::dontSendNotification);
            title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            title.setColour (juce::Label::textColourId, accent());
            title.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (title);

            info.setFont (juce::FontOptions (11.0f));
            info.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            info.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (info);

            if (idx >= 1 && idx <= 3)
            {
                for (auto* b : { &loadBtn, &uiBtn, &delBtn }) addAndMakeVisible (*b);
                loadBtn.onClick = [this] { if (owner.hooks.load) owner.hooks.load (index); };
                uiBtn.onClick   = [this] { if (owner.hooks.editor) owner.hooks.editor (index); };
                delBtn.onClick  = [this] { if (owner.hooks.remove) owner.hooks.remove (index); };

                midi.setTextToShowWhenEmpty ("channels", juce::Colours::grey);
                midi.setFont (juce::FontOptions (12.0f));
                midi.setTooltip ("MIDI channels this track listens to: all, none, or e.g. 1,3,5-8");
                midi.onReturnKey = midi.onFocusLost = [this]
                {
                    filter->setMask (parseChannels (midi.getText()));
                    midi.setText (formatChannels (filter->getMask()), juce::dontSendNotification);
                };
                addAndMakeVisible (midi);
            }

            if (idx != masterIndex)
            {
                solo.setButtonText ("S");
                solo.setClickingTogglesState (true);
                solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0c24a));
                solo.onClick = [this] { strip->setSolo (solo.getToggleState()); if (owner.hooks.soloChanged) owner.hooks.soloChanged(); };
                addAndMakeVisible (solo);
            }

            mute.setButtonText ("M");
            mute.setClickingTogglesState (true);
            mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9534f));
            mute.onClick = [this] { strip->setMuted (mute.getToggleState()); };
            addAndMakeVisible (mute);

            balance.setSliderStyle (juce::Slider::LinearHorizontal);
            balance.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            balance.setRange (-1.0, 1.0, 0.01);
            balance.setDoubleClickReturnValue (true, 0.0);
            balance.setTooltip ("balance (double-click: centre)");
            balance.onValueChange = [this] { strip->setBalance ((float) balance.getValue()); };
            addAndMakeVisible (balance);

            gain.setSliderStyle (juce::Slider::LinearVertical);
            gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
            gain.setRange (-60.0, 12.0, 0.1);
            gain.setSkewFactorFromMidPoint (-12.0);
            gain.setDoubleClickReturnValue (true, 0.0);
            gain.setTextValueSuffix (" dB");
            gain.setTooltip ("gain (double-click: 0 dB)");
            gain.onValueChange = [this] { strip->setGainDb ((float) gain.getValue()); };
            addAndMakeVisible (gain);

            refresh();
        }

        void refresh()
        {
            const auto text = owner.hooks.describe ? owner.hooks.describe (index) : juce::String();
            info.setText (text, juce::dontSendNotification);
            info.setTooltip (text);
            // follow changes made by the control API, unless the user is on the control
            if (! gain.isMouseButtonDown() && std::abs (gain.getValue() - strip->getGainDb()) > 0.05)
                gain.setValue (strip->getGainDb(), juce::dontSendNotification);
            if (! balance.isMouseButtonDown() && std::abs (balance.getValue() - strip->getBalance()) > 0.005)
                balance.setValue (strip->getBalance(), juce::dontSendNotification);
            if (index != masterIndex && solo.getToggleState() != strip->isSolo())
                solo.setToggleState (strip->isSolo(), juce::dontSendNotification);
            if (mute.getToggleState() != strip->isMuted())
                mute.setToggleState (strip->isMuted(), juce::dontSendNotification);
            if (filter != nullptr && ! midi.hasKeyboardFocus (true))
                midi.setText (formatChannels (filter->getMask()), juce::dontSendNotification);
        }

        void tick()
        {
            const float pl = strip->takePeakL(), pr = strip->takePeakR();
            meterL = juce::jmax (pl, meterL * 0.90f);
            meterR = juce::jmax (pr, meterR * 0.90f);
            if (meterL > 1.0f || meterR > 1.0f) clipHold = 45;
            else if (clipHold > 0) --clipHold;
            repaint (meterArea);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (4, 4);
            title.setBounds (r.removeFromTop (16));
            info.setBounds (r.removeFromTop (14));
            r.removeFromTop (2);
            if (filter != nullptr)
            {
                auto b = r.removeFromTop (22);
                loadBtn.setBounds (b.removeFromLeft (b.getWidth() / 2 - 2));
                b.removeFromLeft (2);
                uiBtn.setBounds (b.removeFromLeft (b.getWidth() / 2 - 1));
                b.removeFromLeft (2);
                delBtn.setBounds (b);
                r.removeFromTop (3);
                midi.setBounds (r.removeFromTop (20));
                r.removeFromTop (3);
            }
            auto b = r.removeFromBottom (22);
            mute.setBounds (b.removeFromLeft (24));
            b.removeFromLeft (2);
            if (index != masterIndex) { solo.setBounds (b.removeFromLeft (24)); b.removeFromLeft (3); }
            balance.setBounds (b);
            r.removeFromBottom (3);
            auto m = r.removeFromRight (18);
            meterArea = m.reduced (2, 2);
            gain.setBounds (r);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xff2a2f38));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 5.0f);
            g.setColour (accent().withAlpha (0.35f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 5.0f, 1.0f);

            // meter: two bars (L, R), dB scale -60..+6, red above 0
            g.setColour (juce::Colour (0xff15181d));
            g.fillRect (meterArea);
            const float w = meterArea.getWidth() / 2.0f;
            for (int c = 0; c < 2; ++c)
            {
                const float v = c == 0 ? meterL : meterR;
                const float db = v > 1.0e-4f ? 20.0f * std::log10 (v) : -120.0f;
                const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
                const float h = t * meterArea.getHeight();
                g.setColour (db > 0.0f ? juce::Colour (0xffe0504a) : db > -12.0f ? juce::Colour (0xffe0c24a) : juce::Colour (0xff4ae07a));
                g.fillRect (meterArea.getX() + c * w + 1.0f, meterArea.getBottom() - h, w - 2.0f, h);
            }
            if (clipHold > 0)
            {
                g.setColour (juce::Colour (0xffe0504a));
                g.fillRect (meterArea.getX(), meterArea.getY() - 3, meterArea.getWidth(), 3);
            }
        }

    private:
        juce::Colour accent() const
        {
            static const juce::uint32 c[] = { 0xff5fb0e0, 0xffe0a35f, 0xffe0a35f, 0xffe0a35f, 0xff5fe08a };
            return juce::Colour (c[index]);
        }

        MixerPanel& owner;
        int index;
        TrackStrip* strip;
        MidiChannelFilter* filter;
        juce::Label title, info;
        juce::TextButton loadBtn { "Load" }, uiBtn { "UI" }, delBtn { "x" }, mute, solo;
        juce::TextEditor midi;
        juce::Slider gain, balance;
        juce::Rectangle<int> meterArea;
        float meterL = 0.0f, meterR = 0.0f;
        int clipHold = 0;
    };

    std::unique_ptr<StripView> views[numTracks];
    int slow = 0;
};
