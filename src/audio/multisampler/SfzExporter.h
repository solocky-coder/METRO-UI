#pragma once
// =============================================================================
//  SfzExporter.h  —  MultisamplerInstrument → SFZ text
//  ─────────────────────────────────────────────────────────────────────────
//  Two call sites, same output format (plan §5 "Playback synchronization"
//  and §9 "Export SFZ"):
//    1. The debounced background sync that regenerates a cache-directory SFZ
//       for sfizz after every edit (MultisamplerEngineBridge, Phase 2).
//    2. The user-facing "Export SFZ" bundle action.
//
//  The exporter always emits one fully-resolved <region> per zone — it does
//  not try to reconstruct <global>/<group> inheritance, since the native
//  model has already flattened that distinction away. This keeps the
//  generated file trivial to diff and guarantees sfizz sees exactly what the
//  editor shows, with no separate inheritance logic that could drift from
//  the model (plan §12, "Playback/editor mismatch").
//
//  Only disabled (muted) zones are skipped; everything else in the supported
//  opcode subset round-trips through SfzImporter unchanged.
// =============================================================================

#include "MultisamplerInstrument.h"
#include <juce_core/juce_core.h>

class SfzExporter
{
public:
    struct Options
    {
        /** Write sample= as a path relative to the destination SFZ file's
            folder rather than absolute. Used for portable bundles (plan §9)
            and should stay false for the transient cache-directory file the
            playback sync path writes, since that file's location is
            implementation detail and gains nothing from relative paths. */
        bool useRelativeSamplePaths = false;
    };

    /** Renders `instrument` to SFZ text. `destinationFile` need not exist yet;
        it's only used (when Options::useRelativeSamplePaths is set) to compute
        relative sample= paths, and is not written to by this call. */
    static juce::String render (const MultisamplerInstrument& instrument,
                                 const juce::File& destinationFile,
                                 Options options = {});

    /** Convenience wrapper: renders and writes to `destinationFile` in one
        call. Returns false if the file couldn't be written. */
    static bool exportToFile (const MultisamplerInstrument& instrument,
                               const juce::File& destinationFile,
                               Options options = {});

private:
    SfzExporter() = delete;
};
