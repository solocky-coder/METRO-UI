#include "SfzExporter.h"

namespace
{
    juce::String resolveSamplePath (const SampleZone& z, const juce::File& destinationFile,
                                     const SfzExporter::Options& options)
    {
        if (z.sampleFile == juce::File())
            return {};

        if (options.useRelativeSamplePaths && destinationFile != juce::File())
            return z.sampleFile.getRelativePathFrom (destinationFile.getParentDirectory())
                               .replaceCharacter ('\\', '/');

        return z.sampleFile.getFullPathName().replaceCharacter ('\\', '/');
    }

    void appendOpcode (juce::String& out, const juce::String& key, const juce::String& value)
    {
        if (value.containsChar (' '))
            out << key << "=\"" << value << "\"\n";
        else
            out << key << "=" << value << "\n";
    }

    /** Emits one fully-resolved <region> for `z`. See SfzExporter.h and
        SfzImporter.cpp for the shared conventions this mirrors: SFZ's
        `end`/`loop_end` are the last INCLUDED frame (one less than the
        native model's exclusive sampleEnd/loopEnd), `pan` is written on
        SFZ's -100..100 scale (native model is -1..+1), and `resonance` is
        written on a 0..40 (dB-ish) scale matching this project's existing
        filter opcode convention. */
    void appendRegion (juce::String& out, const SampleZone& z, const juce::File& destinationFile,
                        const SfzExporter::Options& options)
    {
        out << "<region>\n";

        const auto samplePath = resolveSamplePath (z, destinationFile, options);
        if (samplePath.isNotEmpty())
            appendOpcode (out, "sample", samplePath);

        // ── Mapping ──────────────────────────────────────────────────────
        out << "lokey=" << z.lowKey << " hikey=" << z.highKey
            << " pitch_keycenter=" << z.rootKey << "\n";
        out << "lovel=" << z.lowVelocity << " hivel=" << z.highVelocity << "\n";

        // ── Tuning / level ──────────────────────────────────────────────
        if (z.tuneCents != 0.0f) out << "tune="   << juce::String (z.tuneCents, 2) << "\n";
        if (z.gainDb    != 0.0f) out << "volume=" << juce::String (z.gainDb, 2)    << "\n";
        if (z.pan       != 0.0f) out << "pan="    << juce::String (z.pan * 100.0f, 2) << "\n";

        // ── Sample region — see file-header comment re: the -1 endpoint
        //    convention shared with SfzImporter.cpp. ─────────────────────
        if (z.sampleStart > 0)
            out << "offset=" << (juce::int64) z.sampleStart << "\n";
        if (z.sampleEnd >= 0)
            out << "end=" << (juce::int64) (z.sampleEnd - 1) << "\n";

        if (z.loopMode != LoopMode::noLoop)
            out << "loop_mode=" << loopModeToOpcodeValue (z.loopMode) << "\n";
        if (z.loopStart >= 0 && z.loopEnd > z.loopStart)
        {
            out << "loop_start=" << (juce::int64) z.loopStart << "\n";
            out << "loop_end="   << (juce::int64) (z.loopEnd - 1) << "\n";
        }

        // ── Envelope ────────────────────────────────────────────────────
        out << "ampeg_attack="  << juce::String (z.attackSeconds, 4) << "\n";
        out << "ampeg_decay="   << juce::String (z.decaySeconds, 4) << "\n";
        out << "ampeg_sustain=" << juce::String (z.sustainLevel * 100.0f, 2) << "\n";
        out << "ampeg_release=" << juce::String (z.releaseSeconds, 4) << "\n";

        // ── Filter (Lowpass) — only written when actually engaged, i.e.
        //    off its wide-open default, so a flat/untouched zone doesn't
        //    clutter every region with opcodes at their default value. ───
        if (z.filterCutoffHz < 20000.0f)
            out << "cutoff=" << juce::String (z.filterCutoffHz, 1) << "\n";
        if (z.filterResonance > 0.0f)
            out << "resonance=" << juce::String (z.filterResonance * 40.0f, 2) << "\n";

        // ── Per-Zone 3-Band Parametric EQ (sfz eq1_*, eq2_*, eq3_*) ──────
        // Same "only if it does something" gating as the filter above —
        // written only when a band has actually been moved off its default.
        if (z.eqEnabled)
        {
            if (z.eq1Gain != 0.0f || z.eq1Freq != 80.0f)
                out << "eq1_freq=" << juce::String (z.eq1Freq, 1)
                    << " eq1_gain=" << juce::String (z.eq1Gain, 2)
                    << " eq1_bw="   << juce::String (z.eq1Bw, 2) << "\n";

            if (z.eq2Gain != 0.0f || z.eq2Freq != 1000.0f)
                out << "eq2_freq=" << juce::String (z.eq2Freq, 1)
                    << " eq2_gain=" << juce::String (z.eq2Gain, 2)
                    << " eq2_bw="   << juce::String (z.eq2Bw, 2) << "\n";

            if (z.eq3Gain != 0.0f || z.eq3Freq != 8000.0f)
                out << "eq3_freq=" << juce::String (z.eq3Freq, 1)
                    << " eq3_gain=" << juce::String (z.eq3Gain, 2)
                    << " eq3_bw="   << juce::String (z.eq3Bw, 2) << "\n";
        }

        // ── Voice grouping / output routing / round-robin sequencing ────
        if (z.group != 0)     out << "group="  << z.group  << "\n";
        if (z.offBy != 0)     out << "off_by=" << z.offBy  << "\n";
        if (z.outputBus != 1) out << "output="  << z.outputBus << "\n";
        if (z.sequenceLength > 1)
        {
            out << "seq_position=" << z.sequencePosition << "\n";
            out << "seq_length="   << z.sequenceLength   << "\n";
        }

        // ── DYSEKT-native extensions — dysekt_zone_color matches the
        //    existing convention already used for the ZONES engine's own
        //    sfz files (see SfzPlayerDropdownPanel.cpp). ─────────────────
        if (z.hasCustomColour)
            out << "dysekt_zone_color=" << juce::Colour (z.customColourArgb).toString() << "\n";
        // showInMixer now defaults to hidden (SampleZone::showInMixer), so
        // only the SHOWN exception needs an explicit opcode — mirrors the
        // PendingZonePin pass-2 logic in PluginProcessor.cpp, which only
        // ever promotes a zone to true on reimport and never forces false.
        // Writing the opposite branch here (as before) meant a zone the
        // user explicitly turned ON would export nothing, reimport against
        // the (now-correct) hidden-by-default struct value, and silently
        // flip back to hidden on the next engine resync.
        if (z.showInMixer)
            out << "dysekt_show_in_mixer=1\n";
        if (z.reverse)
            out << "dysekt_reverse=1\n";

        // ── Opcodes preserved verbatim from a prior import, outside the
        //    natively-understood subset. ──────────────────────────────────
        for (const auto& [key, value] : z.extraOpcodes)
            appendOpcode (out, key, value);

        out << "\n";
    }
}

juce::String SfzExporter::render (const MultisamplerInstrument& instrument, const juce::File& destinationFile,
                                   Options options)
{
    juce::String out;

    // <curve>/<effect>/<master>/<midi> content the importer can't represent
    // natively — carried through unchanged rather than interpreted (see
    // MultisamplerInstrument::rawExtraHeaders's doc comment).
    for (const auto& header : instrument.rawExtraHeaders)
    {
        out << header;
        if (! header.endsWithChar ('\n'))
            out << "\n";
        out << "\n";
    }

    for (const auto& z : instrument.zones)
    {
        // Only disabled (muted) zones are skipped — see this file's header
        // comment. There's no SFZ opcode for "disabled", so this is the
        // only way to represent it; re-importing an exported file therefore
        // can't recover a zone that was disabled at export time.
        if (! z.enabled)
            continue;

        appendRegion (out, z, destinationFile, options);
    }

    return out;
}

bool SfzExporter::exportToFile (const MultisamplerInstrument& instrument, const juce::File& destinationFile,
                                 Options options)
{
    const auto text = render (instrument, destinationFile, options);
    return destinationFile.replaceWithText (text);
}
