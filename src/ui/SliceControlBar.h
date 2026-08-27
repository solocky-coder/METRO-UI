#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../MidiLearnManager.h"

class DysektProcessor;

class SliceControlBar : public juce::Component,
                        private juce::Timer
{
public:
    explicit SliceControlBar (DysektProcessor& p);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

    // Called by the parent editor's timer — starts/stops the pulse as needed
    void updateMidiLearnPulse();

    // PAD/WAVE view toggle — set externally by the editor and reflected in the button.
    void setPadViewActive (bool on) { padViewActive = on; repaint(); }
    bool getPadViewActive() const noexcept { return padViewActive; }

    /// Fired when the user clicks the PAD/WAVE toggle button.
    std::function<void (bool padActive)> onPadViewToggle;

    // MULTISAMPLER is the permanent content of the SFZ-PLAYER/MULTISAMPLER
    // tab now (see METRO-UI Multisampler Implementation Plan §5.2/5.3) — no
    // more open/close toggle or button. The selected-zone readout strip
    // (loKey/hiKey/root/pitch/pan/volume/release/loop) is now gated purely
    // on isSfzPlayer2Mode(), same as everything else in this class.

    // SAVE used to live here (right-aligned, SFZ-PLAYER/MULTISAMPLER mode
    // only, shown while MULTISAMPLER had staged-but-unsaved edits). It now
    // lives directly on MultisamplerEditor's own toolbar instead — see
    // MultisamplerEditor.h's saveButton doc comment — since the SCB is
    // never shown at all while that tab is active any more.

    /// Resolves the theme colour key represented by whatever's under this
    /// point (a knob's accent fill, the lock icon, a toggle badge...), for
    /// the Theme Editor's PICK mode (see PluginEditor::resolveThemeKeyAt).
    /// Returns an empty string when the cell under the point has no theme
    /// colour of its own — e.g. the ADSR knobs are drawn in fixed, non-theme
    /// colours by design — so the caller can fall back to a general tag.
    juce::String themeKeyAt (juce::Point<int> p) const;

    // Selected-zone readout — SFZ-PLAYER MULTISAMPLER view only. Driven
    // externally by the editor from MultisamplerEditor's zone selection/edit
    // callback, since zones aren't slices and have no selectedSlice/
    // UiSliceSnapshot representation of their own.
    void setSfzZoneSummary (int zoneIndex, const juce::String& name,
                            int loKey, int hiKey, int rootPitch,
                            float tuneCents, float pan, float volDb, float releaseSec, bool isLooped = false,
                            float attackSec = 0.005f, float decaySec = 0.1f, float sustainLevel = 1.0f,
                            float filterCutoffHz = 20000.0f, float filterResonance = 0.0f, int group = 0,
                            // True while this zone is being "layer-auditioned" (right-click
                            // → Edit Layer submenu) — see MultisamplerEditor::layerAuditionZoneId.
                            // Purely cosmetic here: draws an "AUDITIONING" badge next to the
                            // rest of the readout, no different from the loKey/hiKey/etc. fields
                            // it's passed alongside except that it isn't a draggable/editable
                            // SfzZoneField, so it has no entry in that enum and isn't part of
                            // drawSfzZoneSummary()'s drawSfzZoneCell() hit-test loop.
                            bool isAuditioning = false);
    void clearSfzZoneSummary();

    /// Fired after a zone parameter is changed in the SFZ-PLAYER control bar.
    std::function<void (int zoneIndex, int field, float value)> onSfzZoneParamEdited;

public:
    // Phase 4 (SCB ↔ zone wiring coverage pass): ATTACK/DECAY/SUSTAIN/CUTOFF/
    // RESONANCE/GROUP appended after the original 8 fields so existing values
    // (referenced by MultisamplerEditor::applySliceControlBarFieldEdit and any
    // saved layouts) don't shift.
    // ZoneOneShot/ZoneReverse appended after that same coverage pass for the
    // same reason — see SliceLcdDisplay::mouseDown's MULTISAMPLER branch,
    // the only caller that emits these two.
    enum SfzZoneField { ZoneLoKey = -100, ZoneHiKey, ZoneRoot, ZonePitch,
                        ZonePan, ZoneVolume, ZoneRelease, ZoneLoop,
                        ZoneAttack, ZoneDecay, ZoneSustain, ZoneCutoff, ZoneResonance, ZoneGroup,
                        ZoneOneShot, ZoneReverse };
    struct SfzZoneCell { juce::Rectangle<int> bounds; int field; };
private:
    void timerCallback() override;
    float pulsePhase    = 0.0f;   // 0..1, advances each timer tick
    bool  wasArmed      = false;  // tracks arm state across updateMidiLearnPulse calls
    int   lastLiveDrag  = -1;      // last liveDragBoundsStart value seen, for repaint gating
    bool  padViewActive = false;   // mirrors editor showPadGrid
    juce::Rectangle<int> padToggleBtnArea;  // hit-tested in mouseDown — PADS button
    juce::Rectangle<int> waveToggleBtnArea; // hit-tested in mouseDown — WAVE button

    // Selected-zone readout state — see setSfzZoneSummary() doc comment above.
    struct SfzZoneSummary
    {
        bool valid = false;
        int index = -1, loKey = 0, hiKey = 127, rootPitch = -1;
        float tuneCents = 0.0f, pan = 0.0f, volDb = -7.0f, releaseSec = 0.664f;
        bool isLooped = false;
        float attackSec = 0.005f, decaySec = 0.1f, sustainLevel = 1.0f;
        float filterCutoffHz = 20000.0f, filterResonance = 0.0f;
        int group = 0;
        juce::String name;

        // Set from MultisamplerEditor::isZoneBeingAuditioned() via
        // setSfzZoneSummary()'s isAuditioning param — see that param's doc
        // comment. Purely a display flag: drawSfzZoneSummary() paints an
        // "AUDITIONING" badge when true, nothing here drives playback.
        bool isAuditioning = false;
    } sfzZoneSummary;

    // True when the SFZ-PLAYER tab (sliceManager2/voicePool2 — a full second
    // Slicer instance) is the active engine. Mirrors SliceLcdDisplay's
    // identically-named helper. When true, every snapshot/slice/sample-data
    // read below must source from the "2" (engine 2) side, and every pushed
    // Command must set targetEngine2 = true so the audio thread mutates
    // sliceManager2/voicePool2 instead of sliceManager/voicePool.
    bool isSfzPlayer2Mode() const noexcept;

private:
    struct ParamCell
    {
        int x, y, w, h;
        uint32_t lockBit;
        int fieldId; // SliceParamField enum value
        float minVal, maxVal, step;
        bool isBoolean; // for ping-pong toggle
        bool isChoice; // for algorithm popup
        bool isReadOnly = false;
        bool isSetBpm = false;
        bool isMidiLearnBtn = false; // START / END boundary buttons
        bool isKnob = false; // numeric rotary
        bool isMidiLearnable = false; // right-click → Learn menu
        bool isLockIcon = false; // clicking this cell toggles the lock
        float knobNorm = 0.0f; // 0-1 position for knob arc
    };

    std::vector<ParamCell> cells;
    std::vector<SfzZoneCell> sfzZoneCells;
    int activeSfzZoneField = 0;
    bool sfzZoneEditPending = false;

    void drawSfzZoneCell (juce::Graphics&, int x, int y, const juce::String& label,
                          const juce::String& value, int field, int& outWidth);
    void applySfzZoneDrag (int field, float value, bool commit);

    void drawParamCell (juce::Graphics& g, int x, int y, const juce::String& label,
                        const juce::String& value, bool locked, uint32_t lockBit,
                        int fieldId, float minVal, float maxVal, float step,
                        bool isBoolean, bool isChoice, int& outWidth);

    // Rotary knob cell — used for all numeric parameters
    void drawKnobCell (juce::Graphics& g, int x, int y,
                       const juce::String& label, const juce::String& valueText,
                       float normVal, bool locked, uint32_t lockBit,
                       int fieldId, float minVal, float maxVal, float step,
                       int& outWidth);

    // Flat LCD-style slider — used for MARKER (matches TrimDialog IN/OUT style)
    void drawMarkerSliderCell (juce::Graphics& g, int x, int y,
                               int sampleVal, int totalFrames, int& outWidth);

    // Chromatic channel badge — cycles 0 (off) through 1-16 on click
    void drawChroBadgeCell (juce::Graphics& g, int x, int y,
                            int channel, bool locked, int& outWidth);

    // Chromatic legato toggle
    void drawLegatoToggleCell (juce::Graphics& g, int x, int y,
                               bool on, bool locked, int& outWidth);

    // Per-slice "show in MixerPanel" pin/hide toggle — applies in both
    // Slicer and SFZ-PLAYER modes (unlike CHRO/LEGATO, which are Slicer-only).
    void drawMixerToggleCell (juce::Graphics& g, int x, int y,
                              bool on, bool locked, int& outWidth);

    // Horizontal bipolar slider — used for PAN
    void drawPanSliderCell (juce::Graphics& g, int x, int y,
                            float panValue, bool locked, int& outWidth);

    // START / END slice boundary MIDI Learn buttons
    void drawMidiLearnCell (juce::Graphics& g, int x, int y,
                            const juce::String& label, int fieldId, int& outWidth);

    void drawKnob (juce::Graphics& g, int cx, int cy, int r,
                   float normVal, bool locked, bool armed, bool mapped,
                   juce::Colour tintOverride = {}, bool hovered = false);

    void drawLockIcon (juce::Graphics& g, int x, int y, bool locked);
    // PADS/WAVE (Slicer) or MULTI (SFZ-PLAYER) toggle button(s), top-right corner.
    // Independent of per-slice state — must be drawn even when no slice is
    // selected, so MULTI stays reachable on an empty/not-yet-populated kit.
    void drawViewToggleButtons (juce::Graphics& g);
    // Compact per-zone readout drawn in place of the normal slice-param row
    // when the SFZ-PLAYER MULTISAMPLER view is active — see setSfzZoneSummary().
    void drawSfzZoneSummary (juce::Graphics& g, int x, int y, int width, int height) const;
    void showTextEditor (const ParamCell& cell, float currentValue);
    // Zone-field counterpart of showTextEditor() — SfzZoneCell has no lock
    // bit / APVTS default concept (zones aren't Slicer slices), so the
    // commit path is just applySfzZoneDrag(field, value, /*commit=*/true)
    // rather than showTextEditor()'s lock/APVTS-aware branch.
    void showSfzZoneTextEditor (const SfzZoneCell& cell, float currentValue);
    void showMidiLearnMenu (int fieldId, juce::Point<int> screenPos);

    // Per-field helpers
    float getCurrentValue (int fieldId) const;
    float toNorm (int fieldId, float nativeVal) const;
    float fromNorm (int fieldId, float norm) const;

    static constexpr int kKnobR = 9; // knob radius (px)
    float paintSf = 1.0f;
    int psCellW  { 74 };   // kParamCellWidth * paintSf
    int psCellH  { 32 };   // 32 * paintSf
    int psKnobR  { kKnobR };// kKnobR * paintSf       // set at start of paint() — scales to component height

    DysektProcessor& processor;

    // Drag state
    int hoveredCellIdx = -1;   // index into cells[] under cursor, -1 = none
    int activeDragCell = -1;
    float dragStartValue = 0.0f;
    int dragStartY = 0;

    // Snapshot of the cell matched in mouseDown — copied out of cells[] so that
    // paint()'s cells.clear() cannot invalidate the active drag mid-gesture.
    ParamCell activeCellSnapshot {};

    // Fine-mode toggle badge — hit area updated each paint, checked in mouseDown.
    juce::Rectangle<int> markerFineModeToggleArea;

    // Text editor overlay
    std::unique_ptr<juce::TextEditor> textEditor;
};
