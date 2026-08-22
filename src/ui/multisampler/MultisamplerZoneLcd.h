#pragma once
// =============================================================================
//  MultisamplerZoneLcd.h — MULTISAMPLER's own editable zone LCD
//  ─────────────────────────────────────────────────────────────────────────
//  Implementation plan §3.3/§5. Replaces the old route where MULTISAMPLER
//  zone data was mirrored into the shared SliceControlBar (SCB) and edited
//  through SliceControlBar::SfzZoneField. This component:
//
//    - Is an independent juce::Component — NOT derived from SliceLcdDisplay
//      or SliceControlBar, and shares no model state or callbacks with them.
//    - Never owns or mutates a SampleZone. setZoneForDisplay() copies what it
//      needs into a small value snapshot (kept internally) rather than
//      retaining the pointer past the call — see setZoneForDisplay()'s doc
//      comment for why that's a hard requirement here, not a style choice.
//    - Reports edits upward via onFieldEdited using MultisamplerZoneField,
//      not a vector index — MultisamplerEditor is responsible for resolving
//      that back to the correct zone by juce::Uuid (see
//      MultisamplerEditor::applyZoneFieldEdit).
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include "MultisamplerZoneField.h"
#include "../../audio/multisampler/SampleZone.h"

class MultisamplerZoneLcd : public juce::Component
{
public:
    MultisamplerZoneLcd();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Display-only. `zone` is read synchronously here and copied field-by-
        field into an internal value snapshot before this call returns — this
        component never retains `zone` itself. That matters because the
        instrument's zones vector can be mutated (add/delete/reorder/import)
        by unrelated UI activity between paints or across an async drag
        callback; retaining a raw pointer across that gap is a real, exercised
        hazard (delete-selected-zone-mid-edit, import-during-drag), not a
        hypothetical one — see METRO-UI Multisampler Implementation Plan
        §3.3. `displayIndex` is only used for the "ZONE NN" label.
        `isPreview` shows the read-only hover treatment; `isAuditioning`
        shows the same cosmetic badge SliceControlBar::drawSfzZoneSummary
        used to draw for layer-audition zones. */
    void setZoneForDisplay (const SampleZone* zone, int displayIndex,
                             bool isPreview, bool isAuditioning);

    /** No zone to show — draws the empty-state treatment. */
    void clearZone();

    /** Shows the "multiple zones selected" state instead of a field readout. */
    void setMultipleSelection (bool multiple);

    /** Whether the currently-displayed zone accepts drag/click edits. Must
        only be true when the displayed zone IS the selected zone (see
        MultisamplerEditor::resized()'s displayIndex/editable resolution in
        the implementation plan §4) — a hovered-but-not-selected zone is
        always shown read-only regardless of this flag's caller-side intent,
        enforced by MultisamplerEditor only ever passing isPreview=true for
        those and setEditable(false) alongside it. */
    void setEditable (bool shouldEdit);

    /** field/value/isCommit — isCommit is false on every intermediate drag
        frame and true exactly once at gesture end (mouse-up, double-click
        reset, or loop toggle). MultisamplerEditor owns clamping, dirty
        state, refresh, and engine sync; this component only reports raw
        dragged values, pre-clamp, so the two clamp definitions never quietly
        drift apart (see plan §6's table — MultisamplerEditor::
        applyZoneFieldEdit is the single place values are clamped). */
    std::function<void (MultisamplerZoneField field, float value, bool isCommit)> onFieldEdited;

    static constexpr int kPreferredHeight = 76;

private:
    struct Snapshot
    {
        bool valid = false;
        juce::String name;
        int displayIndex = -1;
        int lowKey = 0, highKey = 127, rootKey = 60, group = 0;
        float tuneCents = 0.0f, pan = 0.0f, gainDb = 0.0f;
        float attackSeconds = 0.005f, decaySeconds = 0.1f, sustainLevel = 1.0f, releaseSeconds = 0.1f;
        bool loopOn = false;
        float filterCutoffHz = 20000.0f, filterResonance = 0.0f;
        bool isPreview = false;
        bool isAuditioning = false;
    } snapshot;

    bool multipleSelected = false;
    bool editable = false;

    struct Cell { juce::Rectangle<int> bounds; MultisamplerZoneField field; };
    std::vector<Cell> cells;
    int hoveredCellIdx = -1;

    // Drag state — same shape as SliceControlBar's activeDragCell/
    // dragStartValue/dragStartY, kept local to this component since it no
    // longer shares SliceControlBar's drag machinery.
    bool haveActiveDrag = false;
    MultisamplerZoneField activeField = MultisamplerZoneField::lowKey;
    float dragStartValue = 0.0f;
    int   dragStartY = 0;
    bool  dragMoved = false;

    float getFieldValue (MultisamplerZoneField field) const;
    float defaultValueFor (MultisamplerZoneField field) const;
    float dragScaleFor (MultisamplerZoneField field, bool fineMode) const;
    juce::String formatFieldValue (MultisamplerZoneField field) const;
    juce::String labelFor (MultisamplerZoneField field) const;

    void drawCell (juce::Graphics& g, juce::Rectangle<int> bounds, MultisamplerZoneField field);
    void applyDrag (MultisamplerZoneField field, float rawValue, bool commit);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerZoneLcd)
};
