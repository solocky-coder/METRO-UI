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

    /** Returns a copy of `source` in which every zone with showInMixer set
        has that pin carried over to whichever zone actually WINS each of its
        keys at playback-load time.

        Why this exists: the loader builds one playback slice per MIDI note,
        not one per zone. When several enabled zones cover the same note
        (velocity layers, round-robins, stacked zones), it keeps only the
        LAST <region> in file order for that note and stamps only that
        region's dysekt_show_in_mixer flag on the slice. SfzExporter writes
        enabled zones in list order, so the winner for a note is the last
        enabled zone whose [lowKey, highKey] covers it. A zone with MIX
        shown that is shadowed by a later overlapping zone therefore never
        reaches any slice and no mixer channel is created for it.

        The fix is export-side only: for each key of each pinned zone, the
        winning zone in the exported copy is pinned too. The live model
        (`source`, what the zone LCD shows) is never modified, so the MIX
        cell keeps showing what the user actually set. A pinned zone that
        wins all of its own keys is unchanged, and disabled zones are
        ignored because SfzExporter skips them. Pure function of `source`. */
    inline MultisamplerInstrument withMixerPinsOnWinningZones (const MultisamplerInstrument& source)
    {
        MultisamplerInstrument snapshot = source;
        auto& zones = snapshot.zones;

        for (const auto& pinned : source.zones)
        {
            if (! pinned.enabled || ! pinned.showInMixer)
                continue;

            const int lo = juce::jlimit (0, 127, pinned.lowKey);
            const int hi = juce::jlimit (0, 127, pinned.highKey);

            for (int note = lo; note <= hi; ++note)
            {
                // Last enabled zone covering this note wins (file order).
                for (int i = (int) zones.size() - 1; i >= 0; --i)
                {
                    const auto& z = zones[(size_t) i];
                    if (z.enabled && note >= z.lowKey && note <= z.highKey)
                    {
                        zones[(size_t) i].showInMixer = true;
                        break;
                    }
                }
            }
        }

        return snapshot;
    }
}
