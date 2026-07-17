#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MixerPanel.h"
#include "GlobalEqPanel.h"
#include "ArrangeView.h"

//==============================================================================
//  SlotWindowContent  –  content component (no window chrome of its own)
//
//  Hosts MixerPanel, GlobalEqPanel, and ArrangeView as non-owned children,
//  showing exactly one of them at a time (mirrors the old mutually-exclusive
//  "bottom slot" behaviour, just inside a real window instead of an inline
//  region of the main editor).
//==============================================================================
class SlotWindowContent : public juce::Component
{
public:
    enum class Content { None, Mixer, Eq, Arrange };

    SlotWindowContent (MixerPanel& mixer, GlobalEqPanel& eq, ArrangeView& arrange)
        : mixerPanel (mixer), eqPanel (eq), arrangeView (arrange)
    {
        addChildComponent (mixerPanel);
        addChildComponent (eqPanel);
        addChildComponent (arrangeView);
    }

    /** Show exactly one of Mixer / Eq / Arrange (or None to hide all three). */
    void show (Content c)
    {
        current = c;
        mixerPanel.setVisible  (c == Content::Mixer);
        eqPanel.setVisible     (c == Content::Eq);
        arrangeView.setVisible (c == Content::Arrange);
        resized();
    }

    Content getContent() const noexcept { return current; }

    void resized() override
    {
        auto r = getLocalBounds();
        mixerPanel.setBounds  (r);
        eqPanel.setBounds     (r);
        arrangeView.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF060608));
    }

private:
    MixerPanel&    mixerPanel;
    GlobalEqPanel& eqPanel;
    ArrangeView&   arrangeView;
    Content        current = Content::None;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotWindowContent)
};

//==============================================================================
//  SlotWindow  –  native OS floating window that hosts SlotWindowContent
//
//  Usage (from PluginEditor, standalone builds only):
//      slotWindow.showMixer();     // show / bring to front, Mixer content
//      slotWindow.showEq();        // show / bring to front, EQ content
//      slotWindow.showArrange();   // show / bring to front, Arrange content
//      slotWindow.closeWindow();   // hide
//
//  Mirrors PianoRollWindow's shape: a DocumentWindow with a themed title bar,
//  genuinely floating rather than embedded/laid-out inline inside the main
//  editor's bounds.
//==============================================================================
class SlotWindow : public juce::DocumentWindow
{
public:
    SlotWindow (MixerPanel& mixer, GlobalEqPanel& eq, ArrangeView& arrange,
                juce::LookAndFeel& lnf)
        : juce::DocumentWindow ("Mixer",
                                juce::Colour (0xFF0D0D14),
                                juce::DocumentWindow::closeButton |
                                juce::DocumentWindow::minimiseButton |
                                juce::DocumentWindow::maximiseButton),
          content (mixer, eq, arrange)
    {
        setUsingNativeTitleBar (false);  // use our themed title bar
        setLookAndFeel (&lnf);
        setResizable (true, true);
        setContentNonOwned (&content, true);
        setSize (1000, 560);
        centreWithSize (getWidth(), getHeight());
    }

    ~SlotWindow() override
    {
        setLookAndFeel (nullptr);
    }

    //==========================================================================
    void showMixer()
    {
        setName ("Mixer");
        content.show (SlotWindowContent::Content::Mixer);
        showWindow();
    }

    void showEq()
    {
        setName ("EQ");
        content.show (SlotWindowContent::Content::Eq);
        showWindow();
    }

    void showArrange()
    {
        setName ("Arrange");
        content.show (SlotWindowContent::Content::Arrange);
        showWindow();
    }

    /** Hide the window without destroying it. */
    void closeWindow()
    {
        content.show (SlotWindowContent::Content::None);
        setVisible (false);
    }

    bool isShowingMixer()   const noexcept { return isVisible() && content.getContent() == SlotWindowContent::Content::Mixer; }
    bool isShowingEq()      const noexcept { return isVisible() && content.getContent() == SlotWindowContent::Content::Eq; }
    bool isShowingArrange() const noexcept { return isVisible() && content.getContent() == SlotWindowContent::Content::Arrange; }

    /** Fired when the user hits the native X — lets PluginEditor reset
     *  activeSlot / header-button state to match (see PluginEditor.cpp). */
    std::function<void()> onCloseRequested;

    //==========================================================================
    /** Native X button — hide rather than delete. */
    void closeButtonPressed() override
    {
        closeWindow();
        if (onCloseRequested)
            onCloseRequested();
    }

private:
    void showWindow()
    {
        setVisible (true);
        toFront (true);
    }

    SlotWindowContent content;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotWindow)
};

