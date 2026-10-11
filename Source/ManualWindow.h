#pragma once
#include <JuceHeader.h>
#include "BinaryData.h"

// Scrollable viewer for the embedded user manual (PNG pages), opened from the "?" button.
// The pages are produced from the running instance (see docs/VstTestBench-manual.pdf).
class ManualWindow : public juce::DocumentWindow
{
public:
    explicit ManualWindow (std::function<void()> onClose)
        : juce::DocumentWindow ("VST TestBench \xe4\xbd\xbf\xe7\x94\xa8\xe8\xaa\xac\xe6\x98\x8e\xe6\x9b\xb8",
                                juce::Colours::black, DocumentWindow::allButtons),
          closeCb (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        auto* pages = new Pages();
        auto* vp = new juce::Viewport();
        vp->setViewedComponent (pages, true);
        setContentOwned (vp, false);
        setResizable (false, false);
        centreWithSize (880, 940);
        pages->layout (820);
        setVisible (true);
    }

    void closeButtonPressed() override { if (closeCb) closeCb(); }

private:
    struct Pages : public juce::Component
    {
        juce::Array<juce::Image> pages;
        int contentWidth = 820;

        Pages()
        {
            auto add = [this] (const void* d, int sz)
            {
                auto im = juce::ImageFileFormat::loadFrom (d, (size_t) sz);
                if (im.isValid()) pages.add (im);
            };
            add (BinaryData::manual_p1_png, BinaryData::manual_p1_pngSize);
            add (BinaryData::manual_p2_png, BinaryData::manual_p2_pngSize);
        }

        void layout (int w)
        {
            contentWidth = w;
            int h = 16;
            for (auto& im : pages)
                if (im.getWidth() > 0)
                    h += (int) (w * (double) im.getHeight() / im.getWidth()) + 16;
            setSize (w + 24, h);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff3b3b3b));
            int y = 8;
            for (auto& im : pages)
            {
                if (im.getWidth() <= 0) continue;
                const int ph = (int) (contentWidth * (double) im.getHeight() / im.getWidth());
                g.setColour (juce::Colours::white);
                g.fillRect (12, y, contentWidth, ph);
                g.drawImageWithin (im, 12, y, contentWidth, ph, juce::RectanglePlacement::stretchToFit);
                y += ph + 16;
            }
        }
    };

    std::function<void()> closeCb;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManualWindow)
};
