#pragma once
// =============================================================================
//  MultisamplerInstrument.h  —  Native source-of-truth instrument model
//  ─────────────────────────────────────────────────────────────────────────
//  Owned by the UI thread. Editing this model does not touch audio playback
//  directly — see MultisamplerEngineBridge (Phase 2) for the debounce +
//  background-export + safe-swap path described in the implementation plan,
//  section 5 "Playback synchronization". This header has no audio-thread
//  facing code at all; it is pure data plus UI-thread-only helpers.
// =============================================================================

#include "SampleZone.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <functional>

struct MultisamplerInstrument
{
    /** Bumped whenever a breaking change is made to the on-disk instrument.json
        layout. InstrumentSerializer::load() must keep migration paths for every
        prior value ever shipped. */
    int formatVersion = 1;

    juce::String name;
    juce::String author;

    std::vector<SampleZone> zones;

    float masterGainDb        = 0.0f;
    int   transposeSemitones  = 0;
    float fineTuneCents       = 0.0f;
    int   maxVoices            = 64;

    // ── Zone lifecycle ───────────────────────────────────────────────────

    /** Appends a zone with a fresh id and returns a reference to it. The
        returned reference is only valid until the next call that resizes
        `zones` (add/remove) — callers that need to keep referring to a zone
        across such calls should hold its juce::Uuid instead. */
    SampleZone& addZone (SampleZone zone = {});

    /** Returns true if a zone with this id was found and removed. */
    bool removeZone (const juce::Uuid& id);

    SampleZone*       findZone (const juce::Uuid& id);
    const SampleZone* findZone (const juce::Uuid& id) const;

    /** Deep-copies a zone (new id, same mapping/edits) and inserts it
        immediately after the source zone. Returns nullptr if `id` isn't found. */
    SampleZone* duplicateZone (const juce::Uuid& id);

    // ── Queries used by the mapping editor ──────────────────────────────

    /** All zones that would sound for a given note/velocity, in zones[] order
        (later entries drawn last / on top in the map view). Used both for
        audition preview and for detecting doubled-up regions. */
    std::vector<const SampleZone*> zonesMatching (int midiNote, int velocity) const;

    /** Pairs of zone indices whose key AND velocity rectangles overlap at
        all, regardless of whether they'd ever both sound at once. Drives the
        "overlap" hatching in ZoneMapView. */
    std::vector<std::pair<size_t, size_t>> findOverlappingPairs() const;

    /** Zones whose sampleFile does not currently resolve to a file on disk. */
    std::vector<const SampleZone*> missingSampleZones() const;

    struct ValidationIssue
    {
        enum class Severity { warning, error };
        Severity severity;
        juce::Uuid zoneId;   ///< invalid Uuid() for instrument-level issues
        juce::String message;
    };

    /** Non-destructive pre-save check: missing samples, inverted ranges,
        zero-length loops with looping enabled, etc. Callers decide whether
        `error`-severity issues block the save (per plan §9, "warn before a
        save that would discard unsupported data" applies to import; this
        covers the analogous save-time checks). */
    std::vector<ValidationIssue> validate() const;

    // ── Bundle relocation (plan §9) ──────────────────────────────────────

    /** Points a single zone's sampleFile at a new location. Does not touch
        any other zone, even if several zones shared the missing file. */
    bool relinkSample (const juce::Uuid& id, const juce::File& newFile);

    /** Best-effort: for every zone still missing a sample, looks for a file
        with the same name (case-insensitive) anywhere under `folder`
        (recursively). Returns the number of zones successfully relinked. */
    int relinkAllFromFolder (const juce::File& folder);
};
