#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <deque>
#include <functional>

//==============================================================================
// MIDI input monitor: every message that came in (hardware or injected), which device and channel it came on, and
// which INST tracks took it. The question it answers is "why does this track not hear my keyboard": a note that no
// track took says so in red, with the channel it came on.
//==============================================================================
struct MidiLogEntry
{
    double time = 0.0;          // Time::getMillisecondCounterHiRes() * 0.001
    juce::String source;        // device name, or "(injected)"
    juce::MidiMessage msg;
    juce::uint8 tracks = 0;     // bit t-1 = INST t took it (device and channel matched)
    juce::uint8 emptyTracks = 0;// of those, the ones with no instrument loaded
};

// Any thread writes (the MIDI input thread, the control server), the monitor window reads.
class MidiLog
{
public:
    // Clock / active sensing go to their own short ring: a keyboard that sends clock (48 a second at 120 bpm) filled
    // a shared ring within seconds and pushed every note out of the monitor (seen on the Launchkey, 2026-10-17).
    void add (MidiLogEntry e)
    {
        const int ch = e.msg.isNoteOn() ? e.msg.getChannel() : 0;
        const bool rt = isRealtime (e.msg);
        const juce::SpinLock::ScopedLockType sl (lock);
        if (ch >= 1 && ch <= 16) lastNoteMs[ch - 1] = juce::Time::getMillisecondCounter();
        auto& ring = rt ? realtime : entries;
        ring.push_back (std::move (e));
        while (ring.size() > (rt ? 200u : 1000u)) ring.pop_front();
        if (rt) { ++realtimeCount; if (showRealtime) ++serial; }
        else ++serial;
    }
    std::vector<MidiLogEntry> snapshot (bool includeRealtime) const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::vector<MidiLogEntry> out (entries.begin(), entries.end());
        if (includeRealtime)
        {
            out.insert (out.end(), realtime.begin(), realtime.end());
            std::stable_sort (out.begin(), out.end(), [] (const MidiLogEntry& a, const MidiLogEntry& b) { return a.time < b.time; });
        }
        return out;
    }
    void setShowRealtime (bool b) { const juce::SpinLock::ScopedLockType sl (lock); showRealtime = b; ++serial; }
    juce::uint64 getSerial() const { const juce::SpinLock::ScopedLockType sl (lock); return serial; }
    juce::uint64 getRealtimeCount() const { const juce::SpinLock::ScopedLockType sl (lock); return realtimeCount; }
    juce::uint32 lastNote (int ch) const { const juce::SpinLock::ScopedLockType sl (lock); return lastNoteMs[ch]; }
    void clear() { const juce::SpinLock::ScopedLockType sl (lock); entries.clear(); realtime.clear(); ++serial; }

    // Clock, active sensing, start / stop: a keyboard can send these hundreds of times a second.
    static bool isRealtime (const juce::MidiMessage& m)
    {
        return m.getRawDataSize() == 1 && m.getRawData()[0] >= 0xF8;
    }

    static juce::String describeTracks (const MidiLogEntry& e)
    {
        if (e.msg.getChannel() == 0 && e.tracks == 0) return "-";
        if (e.tracks == 0) return "none (dropped)";
        juce::StringArray t;
        for (int i = 0; i < 3; ++i)
            if (e.tracks & (1 << i))
                t.add ("INST" + juce::String (i + 1) + ((e.emptyTracks & (1 << i)) ? " (empty)" : ""));
        return t.joinIntoString (", ");
    }

private:
    mutable juce::SpinLock lock;
    std::deque<MidiLogEntry> entries, realtime;
    juce::uint64 serial = 0, realtimeCount = 0;
    bool showRealtime = false;
    juce::uint32 lastNoteMs[16] {};
};

//==============================================================================
class MidiMonitorComponent : public juce::Component,
                             private juce::Timer,
                             private juce::TableListBoxModel
{
public:
    // routeInfo: one line per INST track ("INST 1: Launchkey Mini MK4 37 MIDI, ch 1"), shown above the list
    MidiMonitorComponent (MidiLog& l, std::function<juce::StringArray()> routeInfoFn)
        : log (l), routeInfo (std::move (routeInfoFn))
    {
        auto& h = table.getHeader();
        h.addColumn ("time",   1, 90);
        h.addColumn ("input",  2, 210);
        h.addColumn ("ch",     3, 34);
        h.addColumn ("message",4, 250);
        h.addColumn ("to",     5, 170);
        table.setModel (this);
        table.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1b1f25));
        table.setRowHeight (18);
        addAndMakeVisible (table);

        for (auto* b : { &clearBtn, &pauseBtn }) addAndMakeVisible (*b);
        addAndMakeVisible (realtimeBtn);
        pauseBtn.setClickingTogglesState (true);
        clearBtn.onClick = [this] { log.clear(); };
        realtimeBtn.onClick = [this] { log.setShowRealtime (realtimeBtn.getToggleState()); };
        routes.setFont (juce::FontOptions (12.0f));
        routes.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        routes.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (routes);

        setSize (780, 520);
        timerCallback();   // filled when it opens, not one tick later
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff20242b));
        // channel lamps: lit for 300 ms after a note-on on that channel
        auto r = lampArea;
        const int w = r.getWidth() / 16;
        const auto now = juce::Time::getMillisecondCounter();
        for (int ch = 0; ch < 16; ++ch)
        {
            auto cell = r.removeFromLeft (w).reduced (2);
            const auto t = log.lastNote (ch);
            const bool lit = t != 0 && now - t < 300;
            const bool seen = t != 0;
            g.setColour (lit ? juce::Colour (0xff4ae07a) : seen ? juce::Colour (0xff2f5a3d) : juce::Colour (0xff2a2f38));
            g.fillRoundedRectangle (cell.toFloat(), 3.0f);
            g.setColour (lit ? juce::Colours::black : juce::Colours::lightgrey);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (juce::String (ch + 1), cell, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (24);
        clearBtn.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (6);
        pauseBtn.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (10);
        realtimeBtn.setBounds (top.removeFromLeft (220));
        r.removeFromTop (6);
        lampArea = r.removeFromTop (22);
        r.removeFromTop (6);
        routes.setBounds (r.removeFromTop (54));
        r.removeFromTop (4);
        table.setBounds (r);
    }

private:
    void timerCallback() override
    {
        repaint (lampArea);
        // clock / active sensing rate, measured over about a second
        const auto now = juce::Time::getMillisecondCounter();
        if (now - rateFromMs >= 1000)
        {
            const auto c = log.getRealtimeCount();
            realtimeRate = rateFromMs == 0 ? 0.0 : (double) (c - rateFromCount) * 1000.0 / (double) (now - rateFromMs);
            rateFromMs = now;
            rateFromCount = c;
        }
        routes.setText ("Note-on channels (lamps) / where each INST track listens"
                        + (realtimeRate > 0.5 ? "   [clock / active sensing coming in: " + juce::String (juce::roundToInt (realtimeRate)) + " per s]" : juce::String())
                        + ":\n" + routeInfo().joinIntoString ("\n"),
                        juce::dontSendNotification);
        if (pauseBtn.getToggleState()) return;
        const auto s = log.getSerial();
        if (s == lastSerial) return;
        lastSerial = s;
        rows = log.snapshot (realtimeBtn.getToggleState());
        table.updateContent();
        if (! rows.empty()) table.scrollToEnsureRowIsOnscreen ((int) rows.size() - 1);
        table.repaint();
    }

    int getNumRows() override { return (int) rows.size(); }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& e = rows[(size_t) row];
        const bool dropped = e.msg.isNoteOn() && e.tracks == 0;
        g.fillAll (selected ? juce::Colour (0xff3a4250) : dropped ? juce::Colour (0xff4a2626) : (row & 1) ? juce::Colour (0xff1f242b) : juce::Colour (0xff1b1f25));
    }

    void paintCell (juce::Graphics& g, int row, int col, int w, int h, bool) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& e = rows[(size_t) row];
        juce::String text;
        switch (col)
        {
            case 1:   // the message's hi-res counter time as wall-clock time
            {
                const double ago = juce::Time::getMillisecondCounterHiRes() * 0.001 - e.time;
                const auto t = juce::Time::getCurrentTime() - juce::RelativeTime::seconds (ago);
                text = t.formatted ("%H:%M:%S") + "." + juce::String (t.getMilliseconds()).paddedLeft ('0', 3);
                break;
            }
            case 2: text = e.source; break;
            case 3: text = e.msg.getChannel() > 0 ? juce::String (e.msg.getChannel()) : juce::String ("-"); break;
            case 4: text = e.msg.getDescription(); break;
            case 5: text = MidiLog::describeTracks (e); break;
            default: break;
        }
        const bool dropped = col == 5 && e.msg.isNoteOn() && e.tracks == 0;
        g.setColour (dropped ? juce::Colour (0xffff8a7a) : juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (text, 4, 0, w - 6, h, juce::Justification::centredLeft, true);
    }

    MidiLog& log;
    std::function<juce::StringArray()> routeInfo;
    juce::TableListBox table;
    juce::TextButton clearBtn { "Clear" }, pauseBtn { "Pause" };
    juce::ToggleButton realtimeBtn { "Show clock / active sensing" };
    juce::Label routes;
    juce::Rectangle<int> lampArea;
    std::vector<MidiLogEntry> rows;
    juce::uint64 lastSerial = 0, rateFromCount = 0;
    juce::uint32 rateFromMs = 0;
    double realtimeRate = 0.0;
};

class MidiMonitorWindow : public juce::DocumentWindow
{
public:
    MidiMonitorWindow (MidiLog& log, std::function<juce::StringArray()> routeInfo)
        : DocumentWindow ("MIDI Monitor", juce::Colour (0xff20242b), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MidiMonitorComponent (log, std::move (routeInfo)), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};
