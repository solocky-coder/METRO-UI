#pragma once
// =============================================================================
//  MultisamplerZoneField.h — Identifies one editable field of a SampleZone
//  ─────────────────────────────────────────────────────────────────────────
//  Implementation plan §3.2. Previously this identity lived in
//  SliceControlBar::SfzZoneField, which meant MultisamplerEditor (a
//  MULTISAMPLER-owned class) had to include and understand a SLICER-owned
//  header just to know which field it was applying. This enum removes that
//  dependency: MultisamplerZoneLcd and MultisamplerEditor::applyZoneFieldEdit
//  both speak this vocabulary directly, and SliceControlBar/SfzZoneField no
//  longer have any MULTISAMPLER-facing role (see METRO-UI Multisampler
//  Implementation Plan §8 step 7).
// =============================================================================

enum class MultisamplerZoneField
{
    lowKey,
    highKey,
    rootKey,
    tune,
    pan,
    gain,
    attack,
    decay,
    sustain,
    release,
    loopEnabled,
    cutoff,
    resonance,
    group,
    outputBus
};
