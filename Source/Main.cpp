#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "RigJob.h"

class VstTestBenchApplication : public juce::JUCEApplication
{
public:
    VstTestBenchApplication() = default;

    const juce::String getApplicationName() override    { return "VST TestBench"; }
    const juce::String getApplicationVersion() override { return "0.8.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Rig worker: the bench starts this same exe to render one rig job without a window, so that a
        // plugin that crashes takes the worker down and not the bench. See RigWorkerClient.h.
        auto args = juce::StringArray::fromTokens (commandLine, " ", "\"");
        if (args.contains ("--rig-worker") && args.size() >= 3)
        {
            worker = std::make_unique<RigWorker> (juce::File (args[1].unquoted()), juce::File (args[2].unquoted()));
            return;
        }
        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override { mainWindow = nullptr; worker = nullptr; }

    // Loads the plugins on the message thread (left free for their async work), renders on RigRender's
    // thread, writes the result file, then quits. Every phase is logged to <result>.log first.
    class RigWorker
    {
    public:
        RigWorker (const juce::File& jobFile, const juce::File& result)
            : resultFile (result), logFile (result.getSiblingFile (result.getFileName() + ".log"))
        {
            log ("started");
            const auto job = juce::JSON::parse (jobFile.loadFileAsString());
            if (! job.isObject()) { finishWith (fail ("cannot read the job file")); return; }

            juce::addDefaultFormatsToManager (formats);
            juce::String error;
            if (! rigjob::buildSpec (job, formats, spec, error, [this] (const juce::String& m) { log (m); }))
            {
                finishWith (fail (error));
                return;
            }
            spec.progress = [this] (const juce::String& m) { log (m); };
            render.onDone = [this] (juce::var r) { finishWith (std::move (r)); };
            log ("plugins loaded");
            render.start (std::move (spec));
        }

    private:
        static juce::var fail (const juce::String& why)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("ok", false);
            o->setProperty ("error", why);
            return juce::var (o);
        }

        void log (const juce::String& line)
        {
            const juce::ScopedLock sl (lock);
            logFile.appendText (line + juce::newLine);
        }

        void finishWith (juce::var result)
        {
            log ("done");
            const auto tmp = resultFile.getSiblingFile (resultFile.getFileName() + ".tmp");
            tmp.replaceWithText (juce::JSON::toString (result));
            tmp.moveFileTo (resultFile);   // the file appears whole or not at all
            // leave the message thread a moment: the graph hands the plugins back to it for destruction
            juce::Timer::callAfterDelay (400, [] { juce::JUCEApplication::quit(); });
        }

        juce::File resultFile, logFile;
        juce::CriticalSection lock;
        juce::AudioPluginFormatManager formats;
        RigRender::Spec spec;
        RigRender render;
    };

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<RigWorker> worker;
};

START_JUCE_APPLICATION (VstTestBenchApplication)
