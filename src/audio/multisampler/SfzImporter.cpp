#include "SfzImporter.h"
#include <map>
#include <set>
#include <algorithm>

namespace
{
    using OpcodeEntry = std::pair<juce::String, int>;              // (value, line number)
    using OpcodeMap    = std::map<juce::String, OpcodeEntry>;

    enum class HeaderKind { none, global, group, region, control, other };

    // ── Small numeric helpers ───────────────────────────────────────────────

    float floatOpcode (const juce::String& value, float defaultValue)
    {
        if (value.trim().isEmpty())
            return defaultValue;
        return value.getFloatValue();
    }

    int intOpcode (const juce::String& value, int defaultValue)
    {
        if (value.trim().isEmpty())
            return defaultValue;
        return value.getIntValue();
    }

    int64_t int64Opcode (const juce::String& value, int64_t defaultValue)
    {
        if (value.trim().isEmpty())
            return defaultValue;
        return (int64_t) value.getLargeIntValue();
    }

    /** Parses either a plain MIDI note number ("48") or an SFZ note name
        ("C2", "D#-1", "Eb3" ...) into a MIDI note number. App-wide convention
        (see UIHelpers::midiNoteToName): C3 == MIDI note 60, i.e.
        midi = (octave + 2) * 12 + pitchClass. Returns false (leaving
        outValue untouched) if `raw` doesn't parse as either form. */
    bool parseNoteOrInt (const juce::String& raw, int& outValue)
    {
        const auto s = raw.trim();
        if (s.isEmpty())
            return false;

        const auto first = s[0];
        const bool looksLikeNoteName = (first >= 'A' && first <= 'G') || (first >= 'a' && first <= 'g');

        if (! looksLikeNoteName)
        {
            outValue = s.getIntValue();
            return true;
        }

        int semitone = 0;
        switch (juce::CharacterFunctions::toUpperCase (first))
        {
            case 'C': semitone = 0;  break;
            case 'D': semitone = 2;  break;
            case 'E': semitone = 4;  break;
            case 'F': semitone = 5;  break;
            case 'G': semitone = 7;  break;
            case 'A': semitone = 9;  break;
            case 'B': semitone = 11; break;
            default:  return false;
        }

        int idx = 1;
        if (idx < s.length() && (s[idx] == '#' || s[idx] == 's' || s[idx] == 'S'))
        {
            semitone += 1;
            ++idx;
        }
        else if (idx < s.length() && s[idx] == 'b')
        {
            semitone -= 1;
            ++idx;
        }

        const auto octaveStr = s.substring (idx);
        if (octaveStr.isEmpty())
            return false;

        const int octave = octaveStr.getIntValue();
        outValue = (octave + 2) * 12 + semitone;
        return true;
    }

    /** Strips a `//` line comment, respecting (naively) double-quoted spans
        so a quoted path never has its trailing characters eaten. */
    juce::String stripComment (const juce::String& line)
    {
        bool inQuote = false;
        for (int i = 0; i < line.length() - 1; ++i)
        {
            const auto c = line[i];
            if (c == '"')
                inQuote = ! inQuote;
            else if (! inQuote && c == '/' && line[i + 1] == '/')
                return line.substring (0, i);
        }
        return line;
    }

    /** True if `word` looks like the start of its own "key=value" opcode
        (identifier characters up to a non-leading '='). Used to find where
        a greedy, unquoted, space-containing value ends. */
    bool looksLikeOpcodeToken (const juce::String& word)
    {
        const int eq = word.indexOfChar ('=');
        if (eq <= 0)
            return false;

        const auto key = word.substring (0, eq);
        for (auto c : key)
            if (! (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_'))
                return false;
        return true;
    }

    /** Scans every "key=value" opcode out of `lineIn` (which may hold several,
        space-separated — the tail of a header line or a whole opcode line) and
        routes each into the map matching `kind`, or handles it inline if
        `kind == control`. Unquoted values are read greedily, word by word,
        stopping at end of line or as soon as the next word itself looks like
        an opcode — this is what lets an unquoted `sample=` value contain
        spaces (real-world SFZ libraries do this routinely) while still
        allowing more opcodes to follow on the same line. */
    void tokenizeOpcodesInto (const juce::String& lineIn, int lineNumber, HeaderKind kind,
                               OpcodeMap& globalOpcodes, OpcodeMap& groupOpcodes, OpcodeMap& regionOpcodes,
                               juce::String& defaultPath, bool& controlWarned, int controlTagLine,
                               std::vector<SfzImporter::Warning>& warnings)
    {
        juce::String remaining = lineIn;

        for (;;)
        {
            remaining = remaining.trimStart();
            if (remaining.isEmpty())
                break;

            const int eq = remaining.indexOfChar ('=');
            if (eq <= 0)
                break; // nothing left that looks like an opcode

            const auto key = remaining.substring (0, eq);
            bool validKey = true;
            for (auto c : key)
                if (! (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_'))
                    { validKey = false; break; }
            if (! validKey)
                break;

            auto afterEq = remaining.substring (eq + 1);
            juce::String value;

            if (afterEq.startsWithChar ('"'))
            {
                const auto afterQuote = afterEq.substring (1);
                const int closeQuote = afterQuote.indexOfChar ('"');
                if (closeQuote >= 0)
                {
                    value = afterQuote.substring (0, closeQuote);
                    remaining = afterQuote.substring (closeQuote + 1);
                }
                else
                {
                    value = afterQuote;
                    remaining = {};
                }
            }
            else
            {
                afterEq = afterEq.trimStart();
                const int len = afterEq.length();
                int cursor = 0;

                while (cursor < len && ! juce::CharacterFunctions::isWhitespace (afterEq[cursor]))
                    ++cursor;
                value = afterEq.substring (0, cursor);

                for (;;)
                {
                    int lookStart = cursor;
                    while (lookStart < len && juce::CharacterFunctions::isWhitespace (afterEq[lookStart]))
                        ++lookStart;
                    if (lookStart >= len)
                    {
                        cursor = lookStart;
                        break;
                    }

                    int wordEnd = lookStart;
                    while (wordEnd < len && ! juce::CharacterFunctions::isWhitespace (afterEq[wordEnd]))
                        ++wordEnd;

                    const auto nextWord = afterEq.substring (lookStart, wordEnd);
                    if (looksLikeOpcodeToken (nextWord))
                    {
                        cursor = lookStart; // leave it for the next opcode
                        break;
                    }

                    value << " " << nextWord;
                    cursor = wordEnd;
                }

                remaining = afterEq.substring (cursor);
            }

            const auto lowerKey = key.trim().toLowerCase();

            if (kind == HeaderKind::control)
            {
                if (lowerKey == "default_path")
                {
                    defaultPath = value;
                }
                else if (! controlWarned)
                {
                    SfzImporter::Warning w;
                    w.kind = SfzImporter::Warning::Kind::unsupportedHeader;
                    w.lineNumber = controlTagLine;
                    w.detail = "<control> opcode '" + lowerKey + "' is not supported and was dropped.";
                    warnings.push_back (w);
                    controlWarned = true;
                }
            }
            else if (kind == HeaderKind::global) globalOpcodes[lowerKey] = { value, lineNumber };
            else if (kind == HeaderKind::group)  groupOpcodes[lowerKey]  = { value, lineNumber };
            else if (kind == HeaderKind::region) regionOpcodes[lowerKey] = { value, lineNumber };
            // HeaderKind::none / other: no owning zone for this opcode; ignore.
        }
    }

    /** Builds one SampleZone from a region's fully-cascaded (global -> group ->
        region, most specific wins) opcode map and appends it to `instrument`. */
    void buildZoneFromOpcodes (const OpcodeMap& merged, const juce::File& baseDirectory,
                                const juce::String& defaultPath, MultisamplerInstrument& instrument,
                                std::vector<SfzImporter::Warning>& warnings)
    {
        SampleZone z;
        std::set<juce::String> consumed;

        const auto has = [&] (const char* k) { return merged.find (k) != merged.end(); };
        const auto get = [&] (const char* k) -> const juce::String&
        {
            static const juce::String empty;
            const auto it = merged.find (k);
            return it == merged.end() ? empty : it->second.first;
        };
        const auto lineOf = [&] (const char* k) -> int
        {
            const auto it = merged.find (k);
            return it == merged.end() ? -1 : it->second.second;
        };
        const auto consume = [&] (const char* k) { consumed.insert (k); };

        // ── Sample path (honouring a <control> default_path=, if any) ──────
        if (has ("sample"))
        {
            consume ("sample");
            const auto raw = get ("sample").replaceCharacter ('\\', '/');

            juce::File resolved;
            if (juce::File::isAbsolutePath (raw))
            {
                resolved = juce::File (raw);
            }
            else if (defaultPath.isNotEmpty())
            {
                auto dp = defaultPath.replaceCharacter ('\\', '/');
                while (dp.endsWithChar ('/'))
                    dp = dp.dropLastCharacters (1);
                const auto dir = baseDirectory != juce::File() ? baseDirectory.getChildFile (dp) : juce::File (dp);
                resolved = dir.getChildFile (raw);
            }
            else if (baseDirectory != juce::File())
            {
                resolved = baseDirectory.getChildFile (raw);
            }
            else
            {
                resolved = juce::File (raw);
            }
            z.sampleFile = resolved;

            // Only meaningful when we have a real directory to resolve
            // against — an empty/unknown baseDirectory can't tell us
            // anything reliable about whether the file "exists".
            if (baseDirectory != juce::File() && baseDirectory.isDirectory() && ! resolved.existsAsFile())
            {
                SfzImporter::Warning w;
                w.kind = SfzImporter::Warning::Kind::missingSample;
                w.lineNumber = lineOf ("sample");
                w.detail = "sample=" + raw + " could not be found.";
                warnings.push_back (w);
            }
        }

        // ── Mapping — explicit lokey/hikey/pitch_keycenter beat key= ──────
        const bool hasLo = has ("lokey");
        const bool hasHi = has ("hikey");
        const bool hasPk = has ("pitch_keycenter");
        int noteValue = 0;

        if (hasLo)
        {
            if (parseNoteOrInt (get ("lokey"), noteValue)) z.lowKey = noteValue;
            consume ("lokey");
        }
        if (hasHi)
        {
            if (parseNoteOrInt (get ("hikey"), noteValue)) z.highKey = noteValue;
            consume ("hikey");
        }
        if (hasPk)
        {
            if (parseNoteOrInt (get ("pitch_keycenter"), noteValue)) z.rootKey = noteValue;
            consume ("pitch_keycenter");
        }
        if (has ("key"))
        {
            consume ("key");
            if (parseNoteOrInt (get ("key"), noteValue))
            {
                if (! hasLo) z.lowKey  = noteValue;
                if (! hasHi) z.highKey = noteValue;
                if (! hasPk) z.rootKey = noteValue;
            }
        }

        if (has ("lovel")) { z.lowVelocity  = intOpcode (get ("lovel"), z.lowVelocity);  consume ("lovel"); }
        if (has ("hivel")) { z.highVelocity = intOpcode (get ("hivel"), z.highVelocity); consume ("hivel"); }

        // ── Tuning / level ──────────────────────────────────────────────
        if (has ("tune"))   { z.tuneCents = floatOpcode (get ("tune"), z.tuneCents); consume ("tune"); }
        if (has ("volume")) { z.gainDb    = floatOpcode (get ("volume"), z.gainDb);   consume ("volume"); }
        if (has ("pan"))    { z.pan       = floatOpcode (get ("pan"), 0.0f) / 100.0f; consume ("pan"); }

        // ── Sample region — SFZ's `end`/`loop_end` are the last INCLUDED
        //    frame; the native model's sampleEnd/loopEnd are exclusive, one
        //    past the last playable frame. See SfzExporter.cpp's matching -1
        //    on the way out. ─────────────────────────────────────────────
        if (has ("offset")) { z.sampleStart = int64Opcode (get ("offset"), z.sampleStart); consume ("offset"); }
        if (has ("end"))    { z.sampleEnd   = int64Opcode (get ("end"), -1) + 1;           consume ("end"); }
        if (has ("loop_start")) { z.loopStart = int64Opcode (get ("loop_start"), z.loopStart); consume ("loop_start"); }
        if (has ("loop_end"))   { z.loopEnd   = int64Opcode (get ("loop_end"), -1) + 1;        consume ("loop_end"); }
        if (has ("loop_mode"))  { z.loopMode  = loopModeFromOpcodeValue (get ("loop_mode"));   consume ("loop_mode"); }

        // ── Envelope ────────────────────────────────────────────────────
        if (has ("ampeg_attack"))  { z.attackSeconds  = floatOpcode (get ("ampeg_attack"), z.attackSeconds);   consume ("ampeg_attack"); }
        if (has ("ampeg_decay"))   { z.decaySeconds    = floatOpcode (get ("ampeg_decay"), z.decaySeconds);     consume ("ampeg_decay"); }
        if (has ("ampeg_sustain")) { z.sustainLevel     = floatOpcode (get ("ampeg_sustain"), z.sustainLevel * 100.0f) / 100.0f; consume ("ampeg_sustain"); }
        if (has ("ampeg_release")) { z.releaseSeconds   = floatOpcode (get ("ampeg_release"), z.releaseSeconds); consume ("ampeg_release"); }

        // ── Filter (lowpass) ────────────────────────────────────────────
        if (has ("cutoff"))    { z.filterCutoffHz  = floatOpcode (get ("cutoff"), z.filterCutoffHz);   consume ("cutoff"); }
        if (has ("resonance")) { z.filterResonance = floatOpcode (get ("resonance"), 0.0f) / 40.0f;    consume ("resonance"); }

        // ── Per-zone 3-band parametric EQ ───────────────────────────────
        // (Matches this project's existing eq1/eq2/eq3 opcode handling.)
        if (has ("eq1_freq")) { z.eq1Freq = floatOpcode (get ("eq1_freq"), 80.0f);   consume ("eq1_freq"); }
        if (has ("eq1_gain")) { z.eq1Gain = floatOpcode (get ("eq1_gain"), 0.0f);    consume ("eq1_gain"); }
        if (has ("eq1_bw"))   { z.eq1Bw   = floatOpcode (get ("eq1_bw"), 1.0f);      consume ("eq1_bw"); }
        if (has ("eq2_freq")) { z.eq2Freq = floatOpcode (get ("eq2_freq"), 1000.0f); consume ("eq2_freq"); }
        if (has ("eq2_gain")) { z.eq2Gain = floatOpcode (get ("eq2_gain"), 0.0f);    consume ("eq2_gain"); }
        if (has ("eq2_bw"))   { z.eq2Bw   = floatOpcode (get ("eq2_bw"), 1.0f);      consume ("eq2_bw"); }
        if (has ("eq3_freq")) { z.eq3Freq = floatOpcode (get ("eq3_freq"), 8000.0f); consume ("eq3_freq"); }
        if (has ("eq3_gain")) { z.eq3Gain = floatOpcode (get ("eq3_gain"), 0.0f);    consume ("eq3_gain"); }
        if (has ("eq3_bw"))   { z.eq3Bw   = floatOpcode (get ("eq3_bw"), 1.0f);      consume ("eq3_bw"); }

        // ── Grouping / routing / round-robin ────────────────────────────
        if (has ("group"))        { z.group            = intOpcode (get ("group"), z.group);                     consume ("group"); }
        if (has ("off_by"))       { z.offBy             = intOpcode (get ("off_by"), z.offBy);                    consume ("off_by"); }
        if (has ("output"))       { z.outputBus         = intOpcode (get ("output"), z.outputBus);                consume ("output"); }
        if (has ("seq_position")) { z.sequencePosition  = intOpcode (get ("seq_position"), z.sequencePosition);   consume ("seq_position"); }
        if (has ("seq_length"))   { z.sequenceLength    = intOpcode (get ("seq_length"), z.sequenceLength);       consume ("seq_length"); }

        // ── DYSEKT-native extensions (dysekt_zone_color matches the
        //    existing convention already used by SfzPlayerDropdownPanel for
        //    the ZONES engine's own sfz files) ──────────────────────────
        if (has ("dysekt_zone_color"))
        {
            const auto colour = juce::Colour::fromString (get ("dysekt_zone_color"));
            z.hasCustomColour  = true;
            z.customColourArgb = ((juce::uint32) colour.getAlpha() << 24)
                                | ((juce::uint32) colour.getRed()   << 16)
                                | ((juce::uint32) colour.getGreen() << 8)
                                | ((juce::uint32) colour.getBlue());
            consume ("dysekt_zone_color");
        }
        if (has ("dysekt_show_in_mixer")) { z.showInMixer = intOpcode (get ("dysekt_show_in_mixer"), 1) != 0; consume ("dysekt_show_in_mixer"); }
        if (has ("dysekt_reverse"))       { z.reverse     = intOpcode (get ("dysekt_reverse"), 0) != 0;       consume ("dysekt_reverse"); }

        // ── Anything left over is outside the supported subset — preserve
        //    it verbatim rather than silently dropping it, and report it. ──
        for (const auto& [k, entry] : merged)
        {
            if (consumed.find (k) != consumed.end())
                continue;

            z.extraOpcodes.emplace_back (k, entry.first);

            SfzImporter::Warning w;
            w.kind = SfzImporter::Warning::Kind::unsupportedOpcode;
            w.lineNumber = entry.second;
            w.detail = k + "=" + entry.first + " is outside the supported opcode set; preserved verbatim.";
            warnings.push_back (w);
        }

        instrument.addZone (std::move (z));
    }

    SfzImporter::Result parseInternal (const juce::String& text, const juce::File& baseDirectory)
    {
        SfzImporter::Result result;
        result.success = true;
        auto& instrument = result.instrument;

        const auto lines = juce::StringArray::fromLines (text);

        HeaderKind currentKind = HeaderKind::none;

        OpcodeMap globalOpcodes, groupOpcodes, regionOpcodes;
        juce::String defaultPath;
        bool controlWarned = false;
        int controlTagLine = -1;

        juce::String otherHeaderTag;
        int otherHeaderStartLine = -1;
        juce::StringArray otherHeaderLines;

        const auto flushRegionIfAny = [&] ()
        {
            if (currentKind != HeaderKind::region)
                return;

            OpcodeMap merged = globalOpcodes;
            for (const auto& kv : groupOpcodes)  merged[kv.first] = kv.second;
            for (const auto& kv : regionOpcodes) merged[kv.first] = kv.second;
            buildZoneFromOpcodes (merged, baseDirectory, defaultPath, instrument, result.warnings);
        };

        const auto flushOtherHeaderIfAny = [&] ()
        {
            if (currentKind != HeaderKind::other)
                return;

            instrument.rawExtraHeaders.push_back (otherHeaderLines.joinIntoString ("\n"));

            SfzImporter::Warning w;
            w.kind = SfzImporter::Warning::Kind::unsupportedHeader;
            w.lineNumber = otherHeaderStartLine;
            w.detail = "<" + otherHeaderTag + "> is not supported and was preserved verbatim, unedited.";
            result.warnings.push_back (w);
            otherHeaderLines.clear();
        };

        for (int i = 0; i < lines.size(); ++i)
        {
            const int lineNumber = i + 1;
            const auto strippedLine = stripComment (lines[i]);
            const auto leftTrimmed = strippedLine.trimStart();

            if (leftTrimmed.startsWithChar ('<'))
            {
                const int close = leftTrimmed.indexOfChar ('>');
                if (close > 0)
                {
                    const auto tag = leftTrimmed.substring (1, close).trim().toLowerCase();
                    const auto rest = leftTrimmed.substring (close + 1);

                    flushRegionIfAny();
                    flushOtherHeaderIfAny();

                    if (tag == "global")
                    {
                        globalOpcodes.clear();
                        currentKind = HeaderKind::global;
                    }
                    else if (tag == "group")
                    {
                        groupOpcodes.clear();
                        currentKind = HeaderKind::group;
                    }
                    else if (tag == "region")
                    {
                        regionOpcodes.clear();
                        currentKind = HeaderKind::region;
                    }
                    else if (tag == "control")
                    {
                        controlWarned = false;
                        controlTagLine = lineNumber;
                        currentKind = HeaderKind::control;
                    }
                    else
                    {
                        currentKind = HeaderKind::other;
                        otherHeaderTag = tag;
                        otherHeaderStartLine = lineNumber;
                        otherHeaderLines.clear();
                        otherHeaderLines.add (lines[i]);
                    }

                    tokenizeOpcodesInto (rest, lineNumber, currentKind, globalOpcodes, groupOpcodes,
                                         regionOpcodes, defaultPath, controlWarned, controlTagLine, result.warnings);
                    continue;
                }
            }

            if (currentKind == HeaderKind::other)
            {
                otherHeaderLines.add (lines[i]);
                continue;
            }

            if (strippedLine.trim().startsWithIgnoreCase ("#include"))
            {
                SfzImporter::Warning w;
                w.kind = SfzImporter::Warning::Kind::unresolvedInclude;
                w.lineNumber = lineNumber;
                w.detail = strippedLine.trim();
                result.warnings.push_back (w);
                continue;
            }

            tokenizeOpcodesInto (strippedLine, lineNumber, currentKind, globalOpcodes, groupOpcodes,
                                 regionOpcodes, defaultPath, controlWarned, controlTagLine, result.warnings);
        }

        flushRegionIfAny();
        flushOtherHeaderIfAny();

        return result;
    }
}

SfzImporter::Result SfzImporter::importText (const juce::String& sfzText, const juce::File& baseDirectory)
{
    return parseInternal (sfzText, baseDirectory);
}

SfzImporter::Result SfzImporter::importFile (const juce::File& sfzFile)
{
    if (! sfzFile.existsAsFile())
    {
        Result result;
        result.success = false;
        result.errorMessage = "File not found: " + sfzFile.getFullPathName();
        return result;
    }

    return parseInternal (sfzFile.loadFileAsString(), sfzFile.getParentDirectory());
}
