#pragma once
// =============================================================================
//  DrumPadView.h  —  128-pad drum-kit surface for the MULTISAMPLER tab
//  ─────────────────────────────────────────────────────────────────────────
//  8 banks x 16 pads, one pad per MIDI note (bank = note / 16).  Shown in
//  place of ZoneMapView (MultisamplerEditor owns the ZONES | PADS swap).
//
//  This view never mutates the instrument.  It selects and auditions;
//  every edit (bus, colour, mute, delete, drop-a-sample) is reported through
//  a callback so MultisamplerEditor applies it through its normal
//  dirty -> scheduleEngineSync() path.
//
//  Audition + activity use the sfzPlayer2 (MULTISAMPLER engine) paths:
//  processor.pushSfz2UiNote() (lock-free FIFO, per-hit velocity) and
//  processor.sfz2ActiveNotes (also lights from host / arranger MIDI).
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include "../../audio/multisampler/MultisamplerInstrument.h"
#include "../../audio/multisampler/DrumPadModel.h"

class DysektProcessor;

class DrumPadView : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    DrumPadView (DysektProcessor&, MultisamplerInstrument&);
    ~DrumPadView() override;

    /** Recompute the pad model after any zone change. Keeps the bank the user
        is looking at -- never jumps banks mid-edit. */
    void rebuild();

    /** rebuild() + auto-select the bank to show (kit load / kit change). */
    void rebuildAndSelectBank();

    void setUiScale (float s) { uiScale = juce::jlimit (0.65f, 2.0f, s); resized(); }

    int  getSelectedNote() const noexcept { return selectedNote; }
    /** External selection sync (e.g. a zone picked in ZONES). Switches to the
        bank holding `note`; -1 clears. Does not fire onPadSelected. */
    void setSelectedNote (int note);

    int  getBank() const noexcept { return bank; }
    void setBank (int newBank);

    /** Pad model entry for a note (valid after rebuild()). */
    const DrumPadModel::PadInfo& getPad (int note) const { return pads[(size_t) juce::jlimit (0, 127, note)]; }

    /** Stops a held audition note (call before hiding / swapping instruments). */
    void releaseHeldNote();

    // ── Callbacks (all optional, message thread) ─────────────────────────
    /** Pad with a zone was clicked: index into instrument.zones. */
    std::function<void (int zoneIndex)> onPadSelected;
    /** Right-click on any pad (empty or not). */
    std::function<void (int note, juce::Point<int> screenPosition)> onPadContextMenu;
    /** A sample file was dropped on a pad. */
    std::function<void (const juce::File&, int note)> onSampleDropped;

    // juce::Component
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;

    /** -1 = nothing; 0..127 = pad (in the CURRENT bank); <= -100 = bank button. */
    int  hitTest (juce::Point<int>) const;
    static juce::String noteName (int note);
    juce::Colour padColour (const DrumPadModel::PadInfo&) const;
    int  velocityForClick (int padIndexInBank, juce::Point<int>) const;

    DysektProcessor& processor;
    MultisamplerInstrument& instrument;

    DrumPadModel::Model pads {};
    std::array<juce::Rectangle<int>, DrumPadModel::kNumBanks>    bankBounds {};
    std::array<juce::Rectangle<int>, DrumPadModel::kPadsPerBank> padBounds {};
    std::array<bool, DrumPadModel::kNumNotes> active {};

    int   bank         = 0;
    int   heldNote     = -1;
    int   selectedNote = -1;
    int   dropHoverNote = -1;
    float uiScale      = 1.0f;
};
