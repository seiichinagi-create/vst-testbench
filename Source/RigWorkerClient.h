#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>

//==============================================================================
// Runs one rig render in a SEPARATE PROCESS: this same exe started with --rig-worker. Plugin loading,
// prepare, render and teardown all happen over there, so a plugin that crashes (or hangs) takes down
// the worker, not the bench. The worker writes <result>.log as it goes, so a crash report can say what it
// was doing: the last line is the phase (and the plugin being loaded) when it died.
//==============================================================================
class RigWorkerClient : private juce::Thread
{
public:
    RigWorkerClient() : juce::Thread ("rig-worker-client") {}
    ~RigWorkerClient() override
    {
        signalThreadShouldExit();
        stopThread (5000);
    }

    // Fires on the message thread with the result object (the worker's own, or a crash / timeout report).
    std::function<void (juce::var)> onDone;

    bool isRunning() const { return isThreadRunning(); }

    bool start (const juce::var& job, double timeoutSeconds, const juce::File& dir, juce::String& error)
    {
        if (isThreadRunning())
        {
            error = "a rig render is already running";
            return false;
        }
        jobFile    = dir.getChildFile ("rig_job.json");
        resultFile = dir.getChildFile ("rig_result.json");
        logFile    = dir.getChildFile ("rig_result.json.log");
        resultFile.deleteFile();
        logFile.deleteFile();
        if (! jobFile.replaceWithText (juce::JSON::toString (job)))
        {
            error = "cannot write " + jobFile.getFullPathName();
            return false;
        }
        timeout = timeoutSeconds;
        startThread (juce::Thread::Priority::low);
        return true;
    }

private:
    static juce::var obj() { return juce::var (new juce::DynamicObject()); }

    void deliver (juce::var result)
    {
        if (threadShouldExit())
            return;
        auto cb = onDone;
        juce::MessageManager::callAsync ([cb, result] { if (cb != nullptr) cb (result); });
    }

    juce::String lastLogLine() const
    {
        auto lines = juce::StringArray::fromLines (logFile.loadFileAsString());
        lines.removeEmptyStrings();
        return lines.isEmpty() ? juce::String ("(nothing logged: it died before loading anything)") : lines[lines.size() - 1];
    }

    void run() override
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto cmd = exe.getFullPathName().quoted() + " --rig-worker " + jobFile.getFullPathName().quoted()
                         + " " + resultFile.getFullPathName().quoted();

        const double t0 = juce::Time::getMillisecondCounterHiRes();
        juce::ChildProcess child;
        if (! child.start (cmd, 0))   // no pipes: nothing to drain, nothing to block on
        {
            auto r = obj();
            r.getDynamicObject()->setProperty ("ok", false);
            r.getDynamicObject()->setProperty ("error", "cannot start the rig worker: " + exe.getFullPathName());
            deliver (r);
            return;
        }

        bool timedOut = false;
        while (child.isRunning() && ! threadShouldExit())
        {
            if ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 > timeout)
            {
                timedOut = true;
                child.kill();
                break;
            }
            wait (100);
        }
        if (threadShouldExit())
        {
            child.kill();
            return;
        }

        const double seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const auto exitCode = child.getExitCode();

        juce::var result;
        if (! timedOut && resultFile.existsAsFile())
            result = juce::JSON::parse (resultFile.loadFileAsString());

        if (! result.isObject())
        {
            result = obj();
            auto* o = result.getDynamicObject();
            o->setProperty ("ok", false);
            o->setProperty ("worker_crashed", ! timedOut);
            o->setProperty ("error", timedOut
                ? juce::String ("rig worker timed out after ") + juce::String (timeout, 0) + " s, last: " + lastLogLine()
                : "rig worker died (exit code 0x" + juce::String::toHexString ((int) exitCode).toUpperCase()
                  + ") while: " + lastLogLine());
            o->setProperty ("job_file", jobFile.getFullPathName());   // kept so the crash can be replayed
        }
        else
        {
            jobFile.deleteFile();
        }
        result.getDynamicObject()->setProperty ("worker_seconds", seconds);
        deliver (result);
    }

    juce::File jobFile, resultFile, logFile;
    double timeout = 600.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RigWorkerClient)
};
