#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <memory>

//==============================================================================
// Line-delimited JSON over a localhost TCP socket, so an AI (or any script) can
// drive the bench. One request per line:
//     {"id":1,"cmd":"status"}
// One response per line:
//     {"id":1,"ok":true, ...fields...}      or   {"id":1,"ok":false,"error":"..."}
//
// The handler always runs on the JUCE message thread (it touches the graph and
// the UI), so the socket thread only parses, hands off, waits and writes.
// Bound to 127.0.0.1 only.
//==============================================================================
class ControlServer : private juce::Thread
{
public:
    using Handler = std::function<juce::var (const juce::var& request)>;

    explicit ControlServer (Handler h) : juce::Thread ("control-server"), handler (std::move (h)) {}
    ~ControlServer() override { stop(); }

    bool start (int portToUse)
    {
        stop();
        if (! listener.createListener (portToUse, "127.0.0.1"))
            return false;
        port = portToUse;
        startThread (juce::Thread::Priority::low);
        return true;
    }

    void stop()
    {
        signalThreadShouldExit();
        listener.close();
        stopThread (3000);
        port = 0;
    }

    int getPort() const { return port; }

private:
    struct Pending
    {
        juce::var result;
        juce::WaitableEvent done;
    };

    juce::var dispatch (const juce::var& request)
    {
        auto p = std::make_shared<Pending>();
        auto h = handler;
        juce::MessageManager::callAsync ([p, h, request]
        {
            try { p->result = h (request); }
            catch (const std::exception& e)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("error", juce::String ("exception: ") + e.what());
                p->result = juce::var (o);
            }
            p->done.signal();
        });
        if (! p->done.wait (120000))
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("error", "timeout waiting for the message thread (modal dialog open?)");
            return juce::var (o);
        }
        return p->result;
    }

    static void send (juce::StreamingSocket& s, const juce::var& v)
    {
        const auto text = juce::JSON::toString (v, true) + "\n";   // all on one line
        const auto utf8 = text.toUTF8();
        const int total = (int) text.getNumBytesAsUTF8();
        int sent = 0;
        while (sent < total)
        {
            const int n = s.write (utf8.getAddress() + sent, total - sent);
            if (n <= 0) break;
            sent += n;
        }
    }

    juce::var handleLine (const juce::String& line)
    {
        juce::var req;
        const auto parsed = juce::JSON::parse (line, req);
        auto* resp = new juce::DynamicObject();
        juce::var out (resp);
        if (parsed.failed() || ! req.isObject())
        {
            resp->setProperty ("ok", false);
            resp->setProperty ("error", "request is not a JSON object");
            return out;
        }
        if (req.hasProperty ("id"))
            resp->setProperty ("id", req["id"]);

        auto result = dispatch (req);
        if (auto* ro = result.getDynamicObject())
        {
            const bool failed = ro->hasProperty ("error");
            resp->setProperty ("ok", ! failed);
            for (const auto& prop : ro->getProperties())
                resp->setProperty (prop.name, prop.value);
        }
        else
        {
            resp->setProperty ("ok", true);
            resp->setProperty ("result", result);
        }
        return out;
    }

    void serve (juce::StreamingSocket& client)
    {
        std::string pending;
        char buf[4096];
        while (! threadShouldExit() && client.isConnected())
        {
            const int ready = client.waitUntilReady (true, 200);
            if (ready < 0) break;
            if (ready == 0) continue;
            const int n = client.read (buf, (int) sizeof (buf), false);
            if (n <= 0) break;
            pending.append (buf, (size_t) n);

            size_t nl;
            while ((nl = pending.find ('\n')) != std::string::npos)
            {
                const auto line = juce::String::fromUTF8 (pending.data(), (int) nl).trim();
                pending.erase (0, nl + 1);
                if (line.isNotEmpty())
                    send (client, handleLine (line));
            }
        }
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());
            if (client == nullptr)
            {
                if (threadShouldExit()) break;
                wait (100);
                continue;
            }
            serve (*client);
        }
    }

    Handler handler;
    juce::StreamingSocket listener;
    int port = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlServer)
};

//==============================================================================
// Plays timed MIDI into a sink in real time on its own thread (the message
// thread timer is 10 Hz, far too coarse for notes). Used for the control
// server's "midi_play" and for live playback of a MIDI file.
//==============================================================================
class MidiScheduler : private juce::Thread
{
public:
    using Sink = std::function<void (const juce::MidiMessage&)>;

    explicit MidiScheduler (Sink s) : juce::Thread ("midi-scheduler"), sink (std::move (s)) {}
    ~MidiScheduler() override { stop(); }

    // seq timestamps are in seconds from "now". Replaces whatever was playing.
    void play (juce::MidiMessageSequence sequence)
    {
        stop();
        seq = std::move (sequence);
        seq.sort();
        index = 0;
        playing = true;
        startThread (juce::Thread::Priority::high);
    }

    // Stops and releases every sounding note (notes-off + sound-off, all channels).
    void stop()
    {
        stopThread (2000);
        playing = false;
    }

    bool isPlaying() const { return playing.load() && isThreadRunning(); }
    double getDuration() const { return seq.getNumEvents() > 0 ? seq.getEndTime() : 0.0; }
    double getElapsed() const { return elapsed.load(); }

private:
    void run() override
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        while (! threadShouldExit() && index < seq.getNumEvents())
        {
            const auto& m = seq.getEventPointer (index)->message;
            const double due = t0 + m.getTimeStamp() * 1000.0;
            const double now = juce::Time::getMillisecondCounterHiRes();
            elapsed = (now - t0) / 1000.0;
            if (due > now)
            {
                wait ((int) juce::jlimit (1.0, 20.0, due - now));   // never sleep past a stop request
                continue;
            }
            sink (m);
            ++index;
        }
        playing = false;
        if (threadShouldExit())
            for (int ch = 1; ch <= 16; ++ch)
            {
                sink (juce::MidiMessage::allNotesOff (ch));
                sink (juce::MidiMessage::allSoundOff (ch));
            }
    }

    Sink sink;
    juce::MidiMessageSequence seq;
    int index = 0;
    std::atomic<bool> playing { false };
    std::atomic<double> elapsed { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiScheduler)
};
