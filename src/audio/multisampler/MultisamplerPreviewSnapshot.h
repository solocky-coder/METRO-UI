#pragma once
// =============================================================================
//  MultisamplerPreviewSnapshot.h — playback snapshot for layer audition
//  ─────────────────────────────────────────────────────────────────────────
//  MultisamplerEditor::performEngineSync() used to hand SfzExporter a raw
//  copy of `instrument`. That's correct for ordinary editing, but it breaks
//  down the moment several zones overlap the same key/velocity range (a
//  velocity-layered patch, a round-robin stack, two zones a user is mid-way
//  through separating) and the user picks one out via ZoneMapView's
//  right-click "Edit Layer" submenu: every overlapping zone still sounds
//  together, so there is no way to actually hear the one layer being edited
//  in isolation.
//
//  build() is the fix: given the instrument and the id of the zone currently
//  being "layer-auditioned" (see MultisamplerEditor::handleZoneSelectionChanged/
//  clearLayerAudition), it returns the instrument SfzExporter should actually
//  render — identical to `source` except every *other* zone whose key AND
//  velocity range overlaps the audition zone's is disabled. SfzExporter
//  already skips disabled zones (same mechanism the ZONES-map "mute" toggle
//  uses), so this needs no export-side changes at all.
//
//  Deliberately a free function, not a MultisamplerEditor method: it touches
//  no UI-thread-only state (Component, Timer, etc.), so it's trivially safe
//  to call from wherever performEngineSync() builds the background export
//  job's captured snapshot, and trivially unit-testable on its own.
// =============================================================================

#include "MultisamplerInstrument.h"

namespace MultisamplerPreviewSnapshot
{
    /** Pure function of its inputs — no side effects, touches nothing but
        `source` and `auditionZoneId`.

        If `auditionZoneId` doesn't resolve to a zone in `source` (nothing is
        being audited, or the id is stale — e.g. that zone was deleted since),
        returns an unmodified copy of `source`. Otherwise returns a copy where
        every zone other than the audition zone that overlaps its key AND
        velocity range is disabled (SampleZone::enabled = false), so only the
        audited layer sounds for a note that would otherwise trigger several
        stacked zones at once. Zones outside the audition zone's range are
        left exactly as they were — auditioning one layer of a split/
        velocity-layered instrument shouldn't go silent everywhere else on
        the keyboard. */
    inline MultisamplerInstrument build (const MultisamplerInstrument& source,
                                          const juce::Uuid& auditionZoneId)
    {
        if (auditionZoneId == juce::Uuid::null())
            return source;

        const auto* auditionZone = source.findZone (auditionZoneId);
        if (auditionZone == nullptr)
            return source;   // stale id — nothing to solo against

        // Copy the range out before we start mutating `snapshot` below —
        // `auditionZone` points into `source`, not `snapshot`, but taking no
        // copy and comparing against *auditionZone inside the loop would
        // still be fine too; done this way mainly so the overlap test reads
        // as comparing two plain ranges rather than reaching through a
        // pointer on every iteration.
        const int loKey = auditionZone->lowKey,  hiKey = auditionZone->highKey;
        const int loVel = auditionZone->lowVelocity, hiVel = auditionZone->highVelocity;

        MultisamplerInstrument snapshot = source;
        for (auto& z : snapshot.zones)
        {
            if (z.id == auditionZoneId)
                continue;

            const bool keyOverlap = z.lowKey <= hiKey && z.highKey >= loKey;
            const bool velOverlap = z.lowVelocity <= hiVel && z.highVelocity >= loVel;
            if (keyOverlap && velOverlap)
                z.enabled = false;
        }

        return snapshot;
    }
}
