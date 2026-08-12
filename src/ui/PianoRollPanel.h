#pragma once
#include <map>
#include <juce_gui_basics/juce_gui_basics.h>
#include "TransportBar.h"
#include "PianoRollComponent.h"
#include "../sequencer/SequencerEngine.h"
#include "../metro/MetroStepSequencer.h"

namespace
{
    /** Same drum-vs-instrument test MetroArrangeWorkspace/MetroStepSequencer
     *  use to decide between a 16-pad drum mapping and a chromatic note
     *  range — duplicated here (rather than pulled in from metro/) so
     *  PianoRollPanel doesn't take on a dependency on the rest of the Metro
     *  UI just to reuse this one small check. */
    bool pianoRollPanel_isDrumStyleTrack (const SequencerTrackInfo& info) noexcept
    {
        const bool isMainTrack = info.type == TrackType::MainSlice;
        const bool isDrumKit = info.type == TrackType::SfPlayer
                                 && ! info.isSfzInstrument
                                 && info.preset.bank == 128;
        return isMainTrack || isDrumKit;
    }
}

//==============================================================================
//  PianoRollPanel  –  content component (no window chrome of its own)
//
//  Layout:
//    ┌─────────────────────────────────────────────────────┐
//    │  TransportBar                                        │
//    ├───────────────────────────────────────────────────────┤
//    │  PIANO ROLL | STEPS  (tab strip)                      │
//    ├───────────────────────────────────────────────────────┤
//    │  PianoRollComponent  or  MetroStepSequencer —          │
//    │  whichever mode is active for the current track;      │
//    │  both edit the same MidiClip through SequencerEngine  │
//    └─────────────────────────────────────────────────────┘
//
//  Track selection happens in ArrangeView; this window always opens already
//  scoped to a track/clip via openFor(), so it doesn't duplicate a track list.
//
//  Hosted inside PianoRollWindow (a DocumentWindow) so the OS supplies
//  the real title bar with a native X / close button.
//==============================================================================
class PianoRollPanel : public juce::Component
{
public:
    static constexpr int kTransportH  = 38;
    static constexpr int kTabBarH     = 24;

    enum class EditorMode { pianoRoll, steps };

    PianoRollPanel (SequencerEngine& seq, AbletonLink* link = nullptr)
        : engine (seq), transport (seq, link), pianoRoll (seq), stepSequencer (seq)
    {
        addAndMakeVisible (transport);
        addAndMakeVisible (pianoRoll);
        addAndMakeVisible (stepSequencer);

        auto setUpTab = [this] (juce::TextButton& b)
        {
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xFF25D9D9));
            addAndMakeVisible (b);
        };
        setUpTab (pianoRollTabButton);
        setUpTab (stepsTabButton);
        pianoRollTabButton.onClick = [this] { setEditorMode (EditorMode::pianoRoll); };
        stepsTabButton.onClick     = [this] { setEditorMode (EditorMode::steps); };

        showEditorMode (currentMode);

        engine.addMainTrack();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        transport.setBounds (r.removeFromTop (kTransportH));

        auto tabArea = r.removeFromTop (kTabBarH);
        pianoRollTabButton.setBounds (tabArea.removeFromLeft (juce::jmax (80, tabArea.getWidth() / 6)));
        stepsTabButton.setBounds     (tabArea.removeFromLeft (juce::jmax (60, tabArea.getWidth() / 5)));

        pianoRoll.setBounds     (r);
        stepSequencer.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF080A0D));
    }

    void syncSnap()
    {
        pianoRoll.setSnapTicks (transport.getSnapTicks());
    }

    void setActiveTool (PianoRollComponent::Tool t) { pianoRoll.setActiveTool (t); }

    void setEditorMode (EditorMode mode)
    {
        if (activeTrackIndex >= 0)
            viewModeByTrack[activeTrackIndex] = mode;
        showEditorMode (mode);
    }

    void setActiveTrackPublic (int trackIndex, int clipIndex = 0)
    {
        activeTrackIndex = trackIndex;

        pianoRoll.setActiveTrack (trackIndex, clipIndex);
        stepSequencer.setActiveClip (trackIndex, clipIndex);

        auto it = viewModeByTrack.find (trackIndex);
        const EditorMode mode = (it != viewModeByTrack.end())
                                   ? it->second
                                   : (pianoRollPanel_isDrumStyleTrack (engine.getTrackInfo (trackIndex))
                                        ? EditorMode::steps
                                        : EditorMode::pianoRoll);
        // Remember the (possibly just-defaulted) mode so re-opening this
        // track later stays consistent, same as MetroArrangeWorkspace.
        viewModeByTrack[trackIndex] = mode;
        showEditorMode (mode);
    }

    //==========================================================================
    void onSliceChromaticToggled (int sliceIdx, bool enabled,
                                  int chromaticChannel,
                                  const juce::String& name,
                                  juce::Colour colour)
    {
        if (enabled)
            engine.addChromaticTrack (sliceIdx, chromaticChannel, name, colour);
        else
            engine.removeChromaticTrack (sliceIdx);
    }

    void onSf2Loaded (const std::vector<Sf2PresetInfo>& presets,
                      const juce::Colour* palette, int paletteSize)
    {
        engine.rebuildSfTracks (presets, palette, paletteSize);
    }

    void addOrUpdateSfPresetTrack (const Sf2PresetInfo& preset, int midiChannel1Based,
                                   juce::Colour colour)
    {
        engine.addOrUpdateSfTrackOnChannel (preset, midiChannel1Based - 1, colour);
    }

    void addSfzInstrumentTrack (const juce::String& name, juce::Colour colour)
    {
        // 0-based channel 1 == MIDI channel 2, matching sfzPlayer2's own
        // default channel (see DysektProcessor's sfzPlayer2.setMidiChannel(2)
        // and the sfzPlayer2ChannelMask default). Channel 16 here was never
        // correct — sfzPlayer2 was never listening there.
        engine.addSfzTrack (name, 1, colour);
    }

    SequencerTrackInfo getTrackInfo (int i) const { return engine.getTrackInfo (i); }

    /** Kept for API compatibility with PluginEditor's wiring — no longer fired
     *  internally since the piano roll no longer hosts its own track list
     *  (channel reassignment happens via ArrangeView's TrackHeaderStrip). */
    std::function<void(int trackIndex, int midiChannel1Based)> onSfTrackChannelChanged;

private:
    void showEditorMode (EditorMode mode)
    {
        currentMode = mode;

        // Both editors always hold the same clip selection (set in
        // lock-step in setActiveTrackPublic above) — swapping which one is
        // visible never loses anything, since neither owns note data of
        // its own; both read/write straight through to the same MidiClip.
        pianoRoll.setVisible     (mode == EditorMode::pianoRoll);
        stepSequencer.setVisible (mode == EditorMode::steps);

        updateTabButtonStates();
        resized();
    }

    void updateTabButtonStates()
    {
        pianoRollTabButton.setToggleState (currentMode == EditorMode::pianoRoll, juce::dontSendNotification);
        stepsTabButton.setToggleState     (currentMode == EditorMode::steps,     juce::dontSendNotification);
    }

    SequencerEngine&               engine;
    TransportBar                   transport;
    PianoRollComponent             pianoRoll;
    dysekt::metro::MetroStepSequencer stepSequencer;

    juce::TextButton pianoRollTabButton { "PIANO ROLL" };
    juce::TextButton stepsTabButton     { "STEPS" };

    EditorMode currentMode    = EditorMode::pianoRoll;
    int        activeTrackIndex = -1;
    std::map<int, EditorMode> viewModeByTrack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollPanel)
};

//==============================================================================
//  PianoRollWindow  –  native OS floating window that hosts PianoRollPanel
//
//  Usage (from PluginEditor):
//      pianoRollWindow->openFor (trackIndex, clipIndex);   // show / bring to front
//      pianoRollWindow->closeWindow();                     // hide
//==============================================================================
class PianoRollWindow : public juce::DocumentWindow
{
public:
    PianoRollWindow (SequencerEngine& seq, juce::LookAndFeel& lnf, AbletonLink* link = nullptr)
        : juce::DocumentWindow ("PIANO ROLL  •  MIDI EDITOR",
                                juce::Colour (0xFF0D0D14),
                                juce::DocumentWindow::closeButton |
                                juce::DocumentWindow::minimiseButton |
                                juce::DocumentWindow::maximiseButton),
          panel (seq, link)
    {
        setUsingNativeTitleBar (false);  // use our themed title bar
        setLookAndFeel (&lnf);
        setResizable (true, true);
        setContentNonOwned (&panel, true);
        setSize (1180, 680);
        centreWithSize (getWidth(), getHeight());
        wireCallbacks();
    }

    ~PianoRollWindow() override
    {
        setLookAndFeel (nullptr);
    }

    //==========================================================================
    /** Show the window and focus the given track/clip. */
    void openFor (int trackIndex, int clipIndex = 0)
    {
        panel.setActiveTrackPublic (trackIndex, clipIndex);
        panel.syncSnap();
        setVisible (true);
        toFront (true);
    }

    /** Hide the window without destroying it. */
    void closeWindow()
    {
        setVisible (false);
    }

    //==========================================================================
    //  Forwarded accessors (so PluginEditor can reach panel internals)
    void setActiveTool (PianoRollComponent::Tool t)   { panel.setActiveTool (t); }
    SequencerTrackInfo getTrackInfo (int i) const     { return panel.getTrackInfo (i); }
    void syncSnap()                                    { panel.syncSnap(); }

    void onSliceChromaticToggled (int si, bool en, int ch,
                                  const juce::String& name, juce::Colour col)
    { panel.onSliceChromaticToggled (si, en, ch, name, col); }

    void onSf2Loaded (const std::vector<Sf2PresetInfo>& p,
                      const juce::Colour* pal, int palSz)
    { panel.onSf2Loaded (p, pal, palSz); }

    void addOrUpdateSfPresetTrack (const Sf2PresetInfo& preset,
                                   int midiChannel1Based, juce::Colour colour)
    { panel.addOrUpdateSfPresetTrack (preset, midiChannel1Based, colour); }

    void addSfzInstrumentTrack (const juce::String& name, juce::Colour colour)
    { panel.addSfzInstrumentTrack (name, colour); }

    /** Wire this in PluginEditor after construction to receive SF track channel changes. */
    std::function<void(int trackIndex, int midiChannel1Based)> onSfTrackChannelChanged;

    void wireCallbacks()
    {
        panel.onSfTrackChannelChanged = [this] (int ti, int ch)
        {
            if (onSfTrackChannelChanged) onSfTrackChannelChanged (ti, ch);
        };
    }

    //==========================================================================
    /** Native X button — hide rather than delete. */
    void closeButtonPressed() override
    {
        closeWindow();
    }

private:
    PianoRollPanel panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollWindow)
};
