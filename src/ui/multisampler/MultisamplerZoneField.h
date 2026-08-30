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
    outputBus,
    showInMixer,
    eq1Freq,
    eq1Gain,
    eq1Bw,
    eq2Freq,
    eq2Gain,
    eq2Bw,
    eq3Freq,
    eq3Gain,
    eq3Bw,

    // Sentinel — NOT a real field. Always the last enumerator, so its
    // integer value equals the number of real fields above. Used to size
    // this enum's dedicated MIDI Learn slot range (see kMidiLearnSlotBase
    // below) and by MidiLearnManager.h to derive kMidiLearnNumSlots, so
    // adding a field here automatically grows both without any other file
    // needing a hand-maintained count.
    kCount
};

// =============================================================================
//  MIDI Learn — Multisampler gets its own dedicated slot range
//  ─────────────────────────────────────────────────────────────────────────
//  The app runs the Slicer (engine 1) and SFZ-Player/Multisampler (engine 2)
//  concurrently regardless of which tab has UI focus, and PluginProcessor::
//  processMidi()'s CC dispatch routes each learned field id to exactly one
//  hardcoded target. Sharing a slot between, say, the Slicer's Attack and a
//  Multisampler zone's Attack would mean one physical knob simultaneously
//  drives two unrelated things — the same reason FieldSfzAttack (32) is
//  already kept separate from FieldAttack (3) in PluginProcessor.h. So
//  Multisampler fields get their own range here instead of reusing 0-31.
//
//  kMidiLearnSlotBase is a free constant (NOT a member of
//  MultisamplerZoneField — do not qualify it as
//  MultisamplerZoneField::kMidiLearnSlotBase). It's a plain literal, not
//  derived from PluginProcessor::FieldGlobalMono (the highest slot the
//  Slicer/SFZ-Player side currently uses, at 51), because this header is
//  included before PluginProcessor.h's field enum exists — there is no type
//  visible here yet to derive it from. PluginProcessor.h carries a
//  static_assert right after that enum which catches the case where
//  someone raises FieldGlobalMono without also raising this value.
// =============================================================================
static constexpr int kMidiLearnSlotBase = 52;

/** Maps a MultisamplerZoneField to its dedicated MidiLearnManager slot index. */
constexpr int midiLearnSlotFor (MultisamplerZoneField field) noexcept
{
    return kMidiLearnSlotBase + static_cast<int> (field);
}

/** One past the last Multisampler MIDI Learn slot — also what
    MidiLearnManager.h uses (added to nothing else) as kMidiLearnNumSlots,
    since Multisampler's range is the highest one in use. */
constexpr int kMidiLearnSlotCount = kMidiLearnSlotBase + static_cast<int> (MultisamplerZoneField::kCount);

/** True if `slot` (a MidiLearnManager slot index) falls in Multisampler's
    dedicated range. Used by PluginProcessor::processMidi() to route a
    decoded CC to the Multisampler staging arrays instead of any of the
    Slicer/SFZ-Player field branches. */
constexpr bool isMultisamplerMidiLearnSlot (int slot) noexcept
{
    return slot >= kMidiLearnSlotBase && slot < kMidiLearnSlotCount;
}
