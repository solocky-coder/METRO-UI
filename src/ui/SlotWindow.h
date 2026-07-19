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

        configureViewButton (mixerButton, "MIXER");
        configureViewButton (arrangeButton, "ARRANGER");
        mixerButton.onClick   = [this] { selectView (Content::Mixer); };
        arrangeButton.onClick = [this] { selectView (Content::Arrange); };
        addChildComponent (mixerButton);
        addChildComponent (arrangeButton);
    }

    /** Show exactly one of Mixer / Eq / Arrange (or None to hide all three). */
    void show (Content c)
    {
        current = c;
        mixerPanel.setVisible  (c == Content::Mixer);
        eqPanel.setVisible     (c == Content::Eq);
        arrangeView.setVisible (c == Content::Arrange);

        const bool showSwitcher = c == Content::Mixer || c == Content::Arrange;
        mixerButton.setVisible (showSwitcher);
        arrangeButton.setVisible (showSwitcher);
        updateViewButtonState();
        resized();
    }

    Content getContent() const noexcept { return current; }

    /** Called only when the user selects a view from the window switcher. */
    std::function<void (Content)> onViewSelected;

    void resized() override
    {
        auto r = getLocalBounds();
        constexpr int switcherHeight = 34;
        const auto contentBounds = (current == Content::Mixer || current == Content::Arrange)
                                     ? r.withTrimmedTop (switcherHeight)
                                     : r;
        mixerPanel.setBounds  (contentBounds);
        eqPanel.setBounds     (contentBounds);
        arrangeView.setBounds (contentBounds);

        auto switcher = r.removeFromTop (switcherHeight).reduced (8, 5);
        constexpr int arrangeWidth = 92;
        constexpr int mixerWidth = 70;
        arrangeButton.setBounds (switcher.removeFromRight (arrangeWidth));
        switcher.removeFromRight (4);
        mixerButton.setBounds (switcher.removeFromRight (mixerWidth));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF060608));
        if (current == Content::Mixer || current == Content::Arrange)
        {
            g.setColour (juce::Colour (0xFF2A2A35));
            g.drawHorizontalLine (34, 0.0f, (float) getWidth());
        }
    }

private:
    void configureViewButton (juce::TextButton& button, const juce::String& label)
    {
        button.setButtonText (label);
        button.setTooltip ("Switch to " + label.toLowerCase());
        button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF171720));
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF5B3C8A));
        button.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFB7B4C4));
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void updateViewButtonState()
    {
        mixerButton.setToggleState (current == Content::Mixer, juce::dontSendNotification);
        arrangeButton.setToggleState (current == Content::Arrange, juce::dontSendNotification);
    }

    void selectView (Content c)
    {
        if (current == c)
            return;

        show (c);
        if (onViewSelected)
            onViewSelected (c);
    }

    MixerPanel&    mixerPanel;
    GlobalEqPanel& eqPanel;
    ArrangeView&   arrangeView;
    juce::TextButton mixerButton;
    juce::TextButton arrangeButton;
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
        : juce::DocumentWindow ("MIXER",
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
        content.onViewSelected = [this] (SlotWindowContent::Content selected)
        {
            if (selected == SlotWindowContent::Content::Mixer)
                setName ("MIXER");
            else if (selected == SlotWindowContent::Content::Arrange)
                setName ("ARRANGE  •  TRACKS");

            if (onViewSelected)
                onViewSelected (selected);
        };

        setSize (1180, 680);
        centreWithSize (getWidth(), getHeight());
    }

    ~SlotWindow() override
    {
        setLookAndFeel (nullptr);
    }

    //==========================================================================
    void showMixer()
    {
        setName ("MIXER");
        content.show (SlotWindowContent::Content::Mixer);
        showWindow();
    }

    void showEq()
    {
        setName ("EQ  •  GLOBAL");
        content.show (SlotWindowContent::Content::Eq);
        showWindow();
    }

    void showArrange()
    {
        setName ("ARRANGE  •  TRACKS");
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

    /** Fired when the user uses the in-window Mixer / Arranger switcher. */
    std::function<void (SlotWindowContent::Content)> onViewSelected;

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

