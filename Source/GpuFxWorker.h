#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <string>

//==============================================================================
// Client for the persistent gpufx render worker (gpufx.py --serve).
//
// JSON lines over localhost TCP. TCP (not stdin) because juce::ChildProcess
// cannot write to a child's stdin - and the same protocol later points at a
// remote GPU box unchanged.
//
// One background thread owns the socket:  connect (reusing an externally
// started worker if one is listening, else launching our own), send
// "describe" to fetch the parameter schema, then service render requests.
// Requests are latest-wins: a knob change during a render simply replaces
// the pending one. Callbacks are delivered on the message thread.
//==============================================================================
class GpuFxWorker : private juce::Thread
{
public:
    enum class State { off, starting, connecting, ready, rendering, error };

    GpuFxWorker() : juce::Thread ("gpufx worker") {}
    ~GpuFxWorker() override { shutdown(); }

    static constexpr int defaultPort = 8765;
    static constexpr const char* pythonExe  = "C:\\ComfyUI\\.venv\\Scripts\\python.exe";
    static constexpr const char* scriptPath = "C:\\dev\\gpufx\\gpufx.py";

    // Both fire on the message thread.
    std::function<void (juce::var)> onSchemaReady;
    std::function<void (bool ok, juce::String info, juce::File in, juce::File out)> onRenderDone;

    void start()
    {
        if (! isThreadRunning())
        {
            state = State::starting;
            startThread();
        }
    }

    void requestRender (const juce::File& in, const juce::File& out, const juce::var& params)
    {
        {
            const juce::ScopedLock sl (pendingLock);
            pendingIn = in; pendingOut = out; pendingParams = params;
            hasPending = true;
        }
        notify();
    }

    State getState() const          { return state.load(); }
    bool  hasPendingRequest() const { return hasPending.load(); }
    bool  isIdleReady() const       { return state.load() == State::ready && ! hasPending.load(); }

    juce::String getStatusText() const
    {
        switch (state.load())
        {
            case State::starting:
            case State::connecting: return "GPU: starting worker (torch import takes ~10 s)...";
            case State::rendering:  return "GPU: rendering...";
            case State::error:      { const juce::ScopedLock sl (pendingLock); return "GPU error: " + lastError; }
            case State::ready:      return {};   // owner shows its own "ready" text
            default:                return "GPU FX off";
        }
    }

    void shutdown()
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
        if (weLaunched)
            child.kill();
        socket.close();
    }

private:
    //==========================================================================
    void run() override
    {
        if (! connectOrLaunch())
        {
            if (! threadShouldExit()) state = State::error;
            return;
        }
        if (! fetchSchema())
        {
            state = State::error;
            return;
        }
        state = State::ready;

        while (! threadShouldExit())
        {
            juce::File in, out;
            juce::var params;
            bool got = false;
            {
                const juce::ScopedLock sl (pendingLock);
                if (hasPending.load())
                {
                    in = pendingIn; out = pendingOut; params = pendingParams;
                    hasPending = false;
                    got = true;
                }
            }
            if (! got) { wait (200); continue; }

            state = State::rendering;
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("cmd", "render");
            obj->setProperty ("id",  ++requestId);
            obj->setProperty ("in",  in.getFullPathName());
            obj->setProperty ("out", out.getFullPathName());
            obj->setProperty ("params", params);

            // Long files take a while; renders are ~50-150x realtime, so 15 min
            // of wall clock covers anything the RAM cache ceiling allows.
            const auto resp = rpc (juce::var (obj), 15 * 60 * 1000);
            const bool ok = resp["status"].toString() == "done";
            const juce::String info = ok
                ? juce::String (double (resp["xrt"]), 1) + "x realtime"
                : (resp.isVoid() ? juce::String ("no response from worker")
                                 : resp["message"].toString());

            if (threadShouldExit())
                break;
            if (! socket.isConnected())
            {
                setError ("connection to worker lost");
                state = State::error;
            }
            else
                state = State::ready;

            if (onRenderDone != nullptr)
            {
                auto cb = onRenderDone;
                juce::MessageManager::callAsync ([cb, ok, info, in, out] { cb (ok, info, in, out); });
            }
            if (state.load() == State::error)
                return;
        }

        // Only tear down a worker we spawned; leave externally started ones up.
        if (weLaunched && socket.isConnected())
        {
            sendLine ("{\"cmd\":\"quit\"}");
            juce::Thread::sleep (200);
        }
    }

    //==========================================================================
    bool tryConnect (int timeoutMs)
    {
        socket.close();
        return socket.connect ("127.0.0.1", defaultPort, timeoutMs);
    }

    bool connectOrLaunch()
    {
        if (tryConnect (500))
            return true;   // reuse a worker somebody already started

        const juce::File py (pythonExe), script (scriptPath);
        if (! py.existsAsFile())     { setError (juce::String (pythonExe)  + " not found"); return false; }
        if (! script.existsAsFile()) { setError (juce::String (scriptPath) + " not found"); return false; }

        juce::StringArray args { py.getFullPathName(), script.getFullPathName(),
                                 "--serve", "--port", juce::String (defaultPort) };
        if (! child.start (args, 0))
        {
            setError ("failed to launch gpufx worker process");
            return false;
        }
        weLaunched = true;
        state = State::connecting;

        for (int i = 0; i < 120 && ! threadShouldExit(); ++i)   // ~60 s for torch import
        {
            if (tryConnect (500))
                return true;
            if (! child.isRunning())
            {
                setError ("gpufx worker process died during startup");
                return false;
            }
            wait (500);
        }
        setError ("timed out connecting to gpufx worker");
        return false;
    }

    bool fetchSchema()
    {
        const auto resp = rpc (juce::JSON::parse ("{\"cmd\":\"describe\"}"), 10000);
        if (resp["status"].toString() != "ok")
        {
            setError (resp.isVoid() ? "describe failed (no response)" : resp["message"].toString());
            return false;
        }
        if (onSchemaReady != nullptr)
        {
            auto cb = onSchemaReady;
            juce::MessageManager::callAsync ([cb, resp] { cb (resp); });
        }
        return true;
    }

    //==========================================================================
    juce::var rpc (const juce::var& request, int timeoutMs)
    {
        if (! sendLine (juce::JSON::toString (request, true)))
            return {};
        const auto line = readLine (timeoutMs);
        return line.isEmpty() ? juce::var() : juce::JSON::parse (line);
    }

    bool sendLine (const juce::String& s)
    {
        const juce::String line = s + "\n";       // keep alive while writing
        const char* utf8 = line.toRawUTF8();
        const int   len  = (int) strlen (utf8);
        return socket.write (utf8, len) == len;
    }

    juce::String readLine (int timeoutMs)
    {
        const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        for (;;)
        {
            const auto nl = rxBuffer.find ('\n');
            if (nl != std::string::npos)
            {
                const std::string line = rxBuffer.substr (0, nl);
                rxBuffer.erase (0, nl + 1);
                return juce::String::fromUTF8 (line.c_str(), (int) line.size());
            }
            if (threadShouldExit() || juce::Time::getMillisecondCounterHiRes() > deadline)
                return {};

            const int ready = socket.waitUntilReady (true, 200);
            if (ready < 0)
                return {};
            if (ready > 0)
            {
                char tmp[4096];
                const int n = socket.read (tmp, (int) sizeof (tmp), false);
                if (n <= 0)
                    return {};
                rxBuffer.append (tmp, (size_t) n);
            }
        }
    }

    void setError (const juce::String& e)
    {
        const juce::ScopedLock sl (pendingLock);
        lastError = e;
    }

    //==========================================================================
    juce::StreamingSocket socket;
    juce::ChildProcess    child;
    bool                  weLaunched = false;
    std::string           rxBuffer;
    int                   requestId = 0;

    std::atomic<State> state { State::off };
    std::atomic<bool>  hasPending { false };
    mutable juce::CriticalSection pendingLock;
    juce::File  pendingIn, pendingOut;
    juce::var   pendingParams;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GpuFxWorker)
};
