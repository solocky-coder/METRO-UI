#pragma once
// =============================================================================
//  InstrumentSerializer.h  —  MultisamplerInstrument ↔ instrument.json
//  ─────────────────────────────────────────────────────────────────────────
//  This is the *lossless* round trip (plan §9, Required operations / Save).
//  SfzImporter/SfzExporter round-trip through SFZ's opcode vocabulary, which
//  has no concept of e.g. a separate instrument-level master gain — so an
//  SFZ round trip is playback-equivalent, not byte-for-byte faithful to the
//  model. instrument.json is: every field in MultisamplerInstrument and
//  SampleZone is written and read back exactly, including extraOpcodes
//  preserved from a prior SFZ import.
//
//  This class only knows how to (de)serialize the model to/from a JSON
//  string. It does not know about the surrounding .metrokit folder/zip
//  layout, sample collection, or relinking — that orchestration belongs to
//  a higher-level "bundle manager" added alongside Phase 4 packaging.
//  sampleFile is stored as the bundle-relative path
//  ("Samples/sample-001.wav") when `bundleRoot` is supplied, so bundles stay
//  portable across machines (plan §9, "Use relative paths inside bundles").
// =============================================================================

#include "MultisamplerInstrument.h"
#include <juce_core/juce_core.h>

class InstrumentSerializer
{
public:
    /** Serializes `instrument` to instrument.json text. If `bundleRoot` is a
        real directory, each zone's sampleFile is written relative to it;
        otherwise absolute paths are written (suitable for the plugin-state
        "unsaved instrument" case described in plan §9). */
    static juce::String toJson (const MultisamplerInstrument& instrument,
                                 const juce::File& bundleRoot = {});

    struct LoadResult
    {
        bool success = false;
        MultisamplerInstrument instrument;
        juce::String errorMessage;
    };

    /** Parses instrument.json text back into a model. Relative sampleFile
        entries are resolved against `bundleRoot`; if a referenced file
        doesn't exist, the zone is still created (with that unresolved path)
        so the caller's missing-sample/relink UI can surface it rather than
        the load failing outright. Handles migrating older formatVersion
        values encountered in existing projects. */
    static LoadResult fromJson (const juce::String& jsonText, const juce::File& bundleRoot = {});

private:
    InstrumentSerializer() = delete;
    static void migrateIfNeeded (juce::var& root);
};
