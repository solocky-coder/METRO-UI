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

    /** Whether the currently-displayed zone accepts drag/click edits. Must
        only be true when the displayed zone IS the selected zone (see
        MultisamplerEditor::resized()'s displayIndex/editable resolution in
        the implementation plan §4) — a hovered-but-not-selected zone is
        always shown read-only regardless of this flag's caller-side intent,
        enforced by MultisamplerEditor only ever passing isPreview=true for
        those and setEditable(false) alongside it. */
    void setEditable (bool shouldEdit);

    /** "ZONE NN   name" for whatever setZoneForDisplay()/clearZone() last
        set the internal snapshot to — the exact text paint() used to draw
        in this component's own title row before that row moved out to
        MultisamplerEditor's header toolbar (see MultisamplerEditor::
        zoneTagLabel). Returns an empty string when nothing is currently
        displayed (paint()'s "NO ZONE SELECTED" state); callers should
        blank/hide their own label in that case rather than show stale
        text. */
    juce::String getZoneTitleText() const;

    /** Mirror snapshot.isPreview/isAuditioning so MultisamplerEditor's
        zoneBadgeLabel can reproduce the same PREVIEW/AUDITIONING badge
        that used to be drawn on the right edge of this component's title
        row. Both false whenever nothing is displayed. */
    bool isShowingPreview()     const noexcept { return snapshot.valid && snapshot.isPreview; }
    bool isShowingAuditioning() const noexcept { return snapshot.valid && snapshot.isAuditioning; }

    /** Scales every font size this component draws with, so its text keeps
        pace with the rest of the app's host-window-driven scaling (see
        DysektEditor::resized()'s `sf`/`si()`). 1.0 = the design-time sizes
        this file was authored with. MultisamplerEditor forwards its own
        scale here — see MultisamplerEditor::setUiScale(). */
    void setUiScale (float newScale) { uiScale = newScale; repaint(); }

    /** field/value/isCommit — isCommit is false on every intermediate drag
        frame and true exactly once at gesture end (mouse-up, double-click
        reset, or loop toggle). MultisamplerEditor owns clamping, dirty
        state, refresh, and engine sync; this component only reports raw
        dragged values, pre-clamp, so the two clamp definitions never quietly
        drift apart (see plan §6's table — MultisamplerEditor::
        applyZoneFieldEdit is the single place values are clamped). */
    std::function<void (MultisamplerZoneField field, float value, bool isCommit)> onFieldEdited;

    // Taller than the original 76px flat-text layout — knob cells need
    // three full rows of knob+label+value (SliceControlBar's own psCellH is
    // 32px per row) plus a little breathing room; see MultisamplerEditor::
    // resized(), which reads this constant when it reserves space for this
    // component. No longer includes a title row's worth of height — that
    // row (and its PREVIEW/AUDITIONING badge) moved out to
    // MultisamplerEditor's own header toolbar (zoneTagLabel/zoneBadgeLabel),
    // so this constant shrank by the ~20px that row used to cost. Grew from
    // 80 (two rows) to 120 (three rows) when the EQ1/EQ2/EQ3 row was added.
    static constexpr int kPreferredHeight = 120;

private:
    // Same base knob radius as SliceControlBar::kKnobR — see drawKnobField()
    // in the .cpp for why this used to be a per-cell dynamic size instead.
    static constexpr int kKnobR = 9;

    float uiScale = 1.0f;

    struct Snapshot
    {
        bool valid = false;
        juce::String name;
        int displayIndex = -1;
        int lowKey = 0, highKey = 127, rootKey = 60, group = 0, outputBus = 0;
        float tuneCents = 0.0f, pan = 0.0f, gainDb = 0.0f;
        float attackSeconds = 0.005f, decaySeconds = 0.1f, sustainLevel = 1.0f, releaseSeconds = 0.1f;
        bool loopOn = false;
        bool showInMixer = false;
        float filterCutoffHz = 20000.0f, filterResonance = 0.0f;
        float eq1Freq = 100.0f, eq1Gain = 0.0f, eq1Bw = 1.0f;
        float eq2Freq = 1000.0f, eq2Gain = 0.0f, eq2Bw = 1.0f;
        float eq3Freq = 8000.0f, eq3Gain = 0.0f, eq3Bw = 1.0f;
        bool isPreview = false;
        bool isAuditioning = false;
    } snapshot;

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

    // Rotary knob cell — same visual language as SliceControlBar::drawKnob /
    // drawKnobCell (thin arc track, single accent fill arc, plain indicator
    // line, label above value) so the MULTISAMPLER zone row reads as the
    // same kind of control as the rest of the app instead of the cramped
    // flat text pairs this replaces. There's no MIDI Learn / lock concept
    // here — zones aren't Slicer slices — so this only carries the subset
    // of drawKnobCell's state that applies: hover and active-drag.
    void drawKnobField (juce::Graphics& g, juce::Rectangle<int> bounds, MultisamplerZoneField field, int cellIdx);
    void drawKnobArc (juce::Graphics& g, int cx, int cy, int r, float normVal, bool hovered, bool dragging) const;

    // LOOP is boolean — drawn as a flat toggle badge rather than a knob,
    // same call this component made before for that field.
    void drawLoopToggleCell (juce::Graphics& g, juce::Rectangle<int> bounds, int cellIdx);

    // MIX — show-in-MixerPanel pin/hide toggle, same flat badge treatment
    // as LOOP above (and the same concept as SliceControlBar's own
    // drawMixerToggleCell for a Slicer/SFZ-PLAYER slice).
    void drawMixerToggleCell (juce::Graphics& g, juce::Rectangle<int> bounds, int cellIdx);

    // Native value → 0-1 for the knob arc. Ranges mirror either the field's
    // own documented range in SampleZone.h (tune ±1200ct, cutoff 20Hz..
    // 20kHz log, resonance/sustain 0..1) or, where SampleZone.h leaves a
    // field's practical range unstated (gain, attack, decay, release), the
    // same range SliceControlBar's own knobs already use for the nearest
    // equivalent (FieldVolume, FieldAttack/Decay/Release) — see
    // SliceControlBar::toNorm — so a given field's knob sweeps the same
    // visual arc everywhere it appears in the app.
    float normForField (MultisamplerZoneField field) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerZoneLcd)
};
