#include "SfzImporter.h"
#include "../SfzKeyParsing.h"
#include <map>
#include <set>

namespace
{
    // ── Supported opcode subset (plan §5) ───────────────────────────────────
    // Kept as a set purely so unrecognised opcodes can be routed into
    // extraOpcodes with an O(log n) lookup instead of a long if/else chain.
    const std::set<juce::String>& supportedOpcodes()
    {
        static const std::set<juce::String> s = {
            "sample", "key", "lokey", "hikey", "lovel", "hivel",
            "pitch_keycenter", "tune", "transpose", "volume", "pan",
            "offset", "end", "loop_mode", "loop_start", "loop_end",
            "ampeg_attack", "ampeg_decay", "ampeg_sustain", "ampeg_release",
            "cutoff", "resonance", "group", "off_by", "seq_position", "seq_length",
            "dysekt_zone_color", "dysekt_output_bus", "dysekt_show_in_mixer"
        };
        return s;
    }

    const std::set<juce::String>& skippedHeaders()
    {
        // Headers we intentionally do not import in the first release
        // (plan §5: "macros, includes, keyswitches, curves, and advanced
        // modulation can be scheduled after the core release"). <control>
        // is handled separately below — its default_path= opcode IS read
        // (see resolveSamplePath), everything else in it is skipped like
        // these headers are in full.
        static const std::set<juce::String> s = { "curve", "effect", "master", "midi" };
        return s;
    }

    // ── Raw header preservation ──────────────────────────────────────────
    // These four headers carry real authoring content (curve tables, effect
    // chains, master-bus settings, MIDI CC mappings) that MultisamplerInstrument
    // has nowhere to represent — unlike an unsupported region opcode, there's
    // no per-zone extraOpcodes slot a whole <effect> block could hang off of.
    // Rather than lose that content the first time a file round-trips through
    // MULTISAMPLER, capture each block's exact source text (starting at its
    // '<name...>' tag, running to just before the next header tag or EOF —
    // comments, blank lines, and original formatting all included) and carry
    // it on MultisamplerInstrument::rawExtraHeaders for SfzExporter to
    // re-emit byte-for-byte. Scans the raw, un-tokenized source text directly
    // (rather than reusing `tokens`) specifically so this preserves exact
    // formatting instead of only the semantic content the tokenizer keeps.
    std::vector<juce::String> extractRawHeaderBlocks (const juce::String& sourceText,
                                                        const std::set<juce::String>& headerNames)
    {
        std::vector<juce::String> blocks;
        const int len = sourceText.length();
        int pos = 0;

        while (pos < len)
        {
            const int open = sourceText.indexOfChar (pos, '<');
            if (open < 0) break;
            const int close = sourceText.indexOfChar (open, '>');
            if (close < 0) break;

            const auto tagName = sourceText.substring (open + 1, close).trim().toLowerCase();

            if (headerNames.count (tagName))
            {
                const int nextOpen = sourceText.indexOfChar (close + 1, '<');
                const int blockEnd = (nextOpen >= 0) ? nextOpen : len;

                auto block = sourceText.substring (open, blockEnd);
                // Trim only trailing whitespace (the run-up to the next tag,
                // or EOF) so re-emitted blocks don't accumulate blank lines
                // on every save/reload cycle — everything before that stays
                // untouched, comments included.
                while (block.isNotEmpty()
                       && (block.getLastCharacter() == '\n' || block.getLastCharacter() == '\r'
                           || block.getLastCharacter() == ' '  || block.getLastCharacter() == '\t'))
                    block = block.dropLastCharacters (1);

                if (block.isNotEmpty())
                    blocks.push_back (block);

                pos = blockEnd;
            }
            else
            {
                pos = close + 1;
            }
        }

        return blocks;
    }

    // ── Raw token stream ─────────────────────────────────────────────────
    struct Token
    {
        enum class Type { header, opcode };
        Type type;
        int line = 1;
        juce::String headerName;     // Type::header
        juce::String opcodeKey;      // Type::opcode (always lowercase)
        juce::String opcodeValue;    // Type::opcode (original case preserved)
    };

    /** True if `word` looks like the start of a new "identifier=..." opcode.
        Used to decide, while accumulating an unquoted value that may itself
        contain spaces (sample= paths in particular), where that value ends
        and the next opcode begins. */
    bool looksLikeOpcodeStart (const juce::String& word, juce::String& outKey, juce::String& outValueTail)
    {
        int i = 0;
        const auto n = word.length();
        if (n == 0) return false;

        const auto c0 = word[0];
        if (! (juce::CharacterFunctions::isLetter (c0) || c0 == '_'))
            return false;

        for (i = 1; i < n; ++i)
        {
            const auto c = word[i];
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '_')
                continue;
            break;
        }

        if (i >= n || word[i] != '=')
            return false;

        outKey       = word.substring (0, i).toLowerCase();
        outValueTail = word.substring (i + 1);
        return true;
    }

    /** Strips `//` line comments without disturbing quoted strings, and
        normalises line endings so line-number tracking below is exact. */
    juce::String stripComments (const juce::String& text)
    {
        const auto lines = juce::StringArray::fromLines (text);
        juce::StringArray out;
        for (auto line : lines)
        {
            bool inQuotes = false;
            int cut = -1;
            for (int i = 0; i < line.length() - 1; ++i)
            {
                if (line[i] == '"') inQuotes = ! inQuotes;
                if (! inQuotes && line[i] == '/' && line[i + 1] == '/') { cut = i; break; }
            }
            out.add (cut >= 0 ? line.substring (0, cut) : line);
        }
        return out.joinIntoString ("\n");
    }

    std::vector<Token> tokenize (const juce::String& sourceText, std::vector<SfzImporter::Warning>& warnings)
    {
        const auto text = stripComments (sourceText);
        std::vector<Token> tokens;

        const int len = text.length();
        int pos = 0;
        int line = 1;

        auto peekIsSpace = [&] (int p) { return p < len && juce::CharacterFunctions::isWhitespace (text[p]); };

        while (pos < len)
        {
            const auto c = text[pos];

            if (c == '\n') { ++line; ++pos; continue; }
            if (juce::CharacterFunctions::isWhitespace (c)) { ++pos; continue; }

            if (c == '<')
            {
                const int close = text.indexOfChar (pos, '>');
                if (close < 0)
                {
                    warnings.push_back ({ SfzImporter::Warning::Kind::malformedOpcode, line,
                                           "Unterminated '<' header tag" });
                    break;
                }
                Token t;
                t.type = Token::Type::header;
                t.line = line;
                t.headerName = text.substring (pos + 1, close).trim().toLowerCase();
                tokens.push_back (t);
                for (int i = pos; i < close; ++i) if (text[i] == '\n') ++line;
                pos = close + 1;
                continue;
            }

            // Read one whitespace-delimited word.
            const int opcodeStartLine = line;
            int wordStart = pos;
            while (pos < len && ! juce::CharacterFunctions::isWhitespace (text[pos]) && text[pos] != '<')
                ++pos;
            const auto word = text.substring (wordStart, pos);

            juce::String key, valueTail;
            if (! looksLikeOpcodeStart (word, key, valueTail))
            {
                // Stray token outside any opcode= pattern — ignore quietly
                // unless it plausibly looks like a broken assignment.
                if (word.containsChar ('='))
                    warnings.push_back ({ SfzImporter::Warning::Kind::malformedOpcode, line,
                                           "Could not parse token: " + word });
                continue;
            }

            juce::String value;
            const bool quoted = valueTail.startsWithChar ('"');

            if (quoted)
            {
                juce::String acc = valueTail.substring (1);
                bool closed = acc.containsChar ('"');
                if (closed) acc = acc.upToFirstOccurrenceOf ("\"", false, false);

                while (! closed && pos < len)
                {
                    // consume the next word (or rest of line) as part of the quoted value
                    while (pos < len && juce::CharacterFunctions::isWhitespace (text[pos]))
                    {
                        if (text[pos] == '\n') ++line;
                        ++pos;
                    }
                    int segStart = pos;
                    while (pos < len && text[pos] != '"' && text[pos] != '\n') ++pos;
                    acc << " " << text.substring (segStart, pos);
                    if (pos < len && text[pos] == '"') { closed = true; ++pos; }
                }
                value = acc.trim();
            }
            else
            {
                juce::String acc = valueTail;
                // Greedily absorb following words into the value until the
                // next one looks like a new opcode= or we hit a header/EOF.
                // This is what lets `sample=My Grand Piano.wav lokey=48`
                // resolve to a sample path with an embedded space.
                for (;;)
                {
                    if (pos >= len || text[pos] == '<') break;

                    int lookaheadPos = pos;
                    while (lookaheadPos < len && juce::CharacterFunctions::isWhitespace (text[lookaheadPos]))
                        ++lookaheadPos;
                    if (lookaheadPos >= len || text[lookaheadPos] == '<') break;

                    int wStart = lookaheadPos, wEnd = lookaheadPos;
                    while (wEnd < len && ! juce::CharacterFunctions::isWhitespace (text[wEnd]) && text[wEnd] != '<')
                        ++wEnd;
                    const auto nextWord = text.substring (wStart, wEnd);

                    juce::String nk, nv;
                    if (looksLikeOpcodeStart (nextWord, nk, nv))
                        break;   // next opcode begins here — stop absorbing

                    acc << " " << nextWord;
                    for (int i = pos; i < wEnd; ++i) if (text[i] == '\n') ++line;
                    pos = wEnd;
                }
                value = acc.trim();
            }

            Token t;
            t.type = Token::Type::opcode;
            t.line = opcodeStartLine;
            t.opcodeKey = key;
            t.opcodeValue = value;
            tokens.push_back (t);
        }

        return tokens;
    }

    // ── Opcode context: ordered-override map, last write per key wins ──────
    using OpcodeMap = std::map<juce::String, juce::String>;

    void applyToken (OpcodeMap& map, const Token& t)
    {
        map[t.opcodeKey] = t.opcodeValue;
    }

    float floatOpcode (const OpcodeMap& m, const juce::String& key, float fallback)
    {
        const auto it = m.find (key);
        return it == m.end() ? fallback : it->second.getFloatValue();
    }

    int intOpcode (const OpcodeMap& m, const juce::String& key, int fallback)
    {
        const auto it = m.find (key);
        return it == m.end() ? fallback : it->second.getIntValue();
    }

    /** Like intOpcode(), but for the four key-related opcodes that may hold
        either a raw MIDI number or a note name (see SfzKeyParsing.h). A
        plain intOpcode() on "c3" silently returns 0 — which looks like a
        valid, if surprising, key value rather than a parse failure — so
        this needs its own path rather than reusing intOpcode() with a
        different getIntValue()-alike. Pushes a malformedOpcode warning and
        keeps `fallback` if the value can't be parsed either way. */
    int keyOpcode (const OpcodeMap& m, const juce::String& key, int fallback,
                   int regionIndex, int lineForWarnings, std::vector<SfzImporter::Warning>& warnings)
    {
        const auto it = m.find (key);
        if (it == m.end())
            return fallback;

        const int parsed = SfzKeyParsing::parseKeyValue (it->second);
        if (parsed < 0)
        {
            warnings.push_back ({ SfzImporter::Warning::Kind::malformedOpcode, lineForWarnings,
                                   "Region " + juce::String (regionIndex) + ": couldn't parse "
                                       + key + "=" + it->second + " as a MIDI note or note name" });
            return fallback;
        }
        return parsed;
    }

    int64_t int64Opcode (const OpcodeMap& m, const juce::String& key, int64_t fallback)
    {
        const auto it = m.find (key);
        return it == m.end() ? fallback : (int64_t) it->second.getLargeIntValue();
    }

    /** `defaultPath` is the <control> header's default_path= value (empty if
        none was set). Per the SFZ spec it is unconditionally prepended to
        every relative sample= path from the point it's declared onward — no
        attempt is made to detect "this path already looks like it includes
        the default_path folder" and skip prepending, since that's a string-
        matching heuristic with real false positives/negatives (a sample
        genuinely named to look like the folder; different casing; etc). A
        file that redundantly repeats the folder name in some regions after
        already setting default_path is an authoring inconsistency in that
        file, not something the importer should silently paper over — those
        regions will correctly show up as missing samples, recoverable via
        MultisamplerInstrument::relinkAllFromFolder(). */
    juce::File resolveSamplePath (const juce::String& rawPath, const juce::File& baseDirectory,
                                   const juce::String& defaultPath)
    {
        if (rawPath.isEmpty())
            return {};

        // SFZ paths may use either slash style; normalise to the host's.
        auto normalised = rawPath.replaceCharacter ('\\', '/');

        const juce::File asAbsolute (normalised);
        if (juce::File::isAbsolutePath (normalised))
            return asAbsolute;

        juce::File base = (baseDirectory == juce::File())
                         ? juce::File::getCurrentWorkingDirectory()
                         : baseDirectory;

        if (defaultPath.isNotEmpty())
        {
            auto normalisedDefault = defaultPath.replaceCharacter ('\\', '/');
            while (normalisedDefault.endsWithChar ('/'))
                normalisedDefault = normalisedDefault.dropLastCharacters (1);
            if (normalisedDefault.isNotEmpty())
                base = base.getChildFile (normalisedDefault);
        }

        return base.getChildFile (normalised);
    }

    SampleZone buildZone (const OpcodeMap& resolved, const juce::File& baseDirectory,
                           const juce::String& defaultPath,
                           int regionIndex, std::vector<SfzImporter::Warning>& warnings, int lineForWarnings)
    {
        SampleZone z;
        z.id = juce::Uuid();

        // key= is shorthand for lokey=hikey=key=<same value>, and must be
        // applied before the explicit lokey/hikey checks below so an explicit
        // lokey or hikey on the same region still wins (SFZ semantics: later
        // opcodes on the effective region override earlier ones — here that
        // reduces to "explicit lokey/hikey beat key=" since inheritance has
        // already been flattened into `resolved` in file order).
        //
        // All four of key/lokey/hikey/pitch_keycenter go through keyOpcode()
        // rather than intOpcode() because SFZ allows note-name values here
        // ("c3", "d#-1") — a plain getIntValue() silently returns 0 for
        // those, which previously corrupted every region in a note-named
        // file (or a file mixing key= with note-named lokey/hikey/
        // pitch_keycenter on the same region, as real commercial libraries
        // do) down to key 0 instead of reporting the problem.
        if (resolved.count ("key"))
        {
            const auto k = keyOpcode (resolved, "key", 60, regionIndex, lineForWarnings, warnings);
            z.lowKey = z.highKey = z.rootKey = k;
        }

        z.lowKey  = keyOpcode (resolved, "lokey", z.lowKey, regionIndex, lineForWarnings, warnings);
        z.highKey = keyOpcode (resolved, "hikey", z.highKey, regionIndex, lineForWarnings, warnings);
        z.rootKey = keyOpcode (resolved, "pitch_keycenter", z.rootKey, regionIndex, lineForWarnings, warnings);

        z.lowVelocity  = juce::jlimit (0, 127, intOpcode (resolved, "lovel", 1));
        z.highVelocity = juce::jlimit (0, 127, intOpcode (resolved, "hivel", 127));

        z.tuneCents = floatOpcode (resolved, "tune", 0.0f);
        // `transpose` (whole semitones) folds into tuneCents at the zone
        // level in the native model — MultisamplerInstrument keeps a
        // separate instrument-wide transposeSemitones for the editor's own
        // global control, but a per-region transpose= in an imported file is
        // this region's, not the instrument's, so it must not be lost.
        z.tuneCents += 100.0f * (float) intOpcode (resolved, "transpose", 0);

        z.gainDb = floatOpcode (resolved, "volume", 0.0f);
        z.pan    = juce::jlimit (-1.0f, 1.0f, floatOpcode (resolved, "pan", 0.0f) / 100.0f);

        // `offset`/`loop_start` already mean "first played frame" in both SFZ
        // and the native model, so they copy straight across. `end`/`loop_end`
        // do not: sfizz (like the SFZ spec) treats them as the *last included*
        // frame, while the native model's [start, end) convention (see
        // SampleZone.h) treats sampleEnd/loopEnd as one-past-the-last frame.
        // Converting only at this import boundary (and the matching -1 at
        // export) keeps every other piece of code — the waveform displays,
        // trim UI, validation — working in one consistent exclusive-end
        // convention without needing to know SFZ's opcode semantics at all.
        z.sampleStart = int64Opcode (resolved, "offset", 0);
        {
            const auto importedEnd = int64Opcode (resolved, "end", -1);
            z.sampleEnd = importedEnd < 0 ? -1 : importedEnd + 1;
        }
        z.loopStart = int64Opcode (resolved, "loop_start", -1);
        {
            const auto importedLoopEnd = int64Opcode (resolved, "loop_end", -1);
            z.loopEnd = importedLoopEnd < 0 ? -1 : importedLoopEnd + 1;
        }
        if (resolved.count ("loop_mode"))
            z.loopMode = loopModeFromOpcodeValue (resolved.at ("loop_mode"));

        z.attackSeconds  = juce::jmax (0.0f, floatOpcode (resolved, "ampeg_attack", 0.005f));
        z.decaySeconds   = juce::jmax (0.0f, floatOpcode (resolved, "ampeg_decay", 0.1f));
        z.sustainLevel   = juce::jlimit (0.0f, 100.0f, floatOpcode (resolved, "ampeg_sustain", 100.0f)) / 100.0f;
        z.releaseSeconds = juce::jmax (0.0f, floatOpcode (resolved, "ampeg_release", 0.1f));

        z.filterCutoffHz  = floatOpcode (resolved, "cutoff", 20000.0f);
        // sfz `resonance` is a dB peak (typically 0..40dB); the native model
        // keeps a normalised 0..1 for its filter UI, so remap here.
        z.filterResonance = juce::jlimit (0.0f, 1.0f, floatOpcode (resolved, "resonance", 0.0f) / 40.0f);

        // Custom colour override — see SfzExporter for the write side.
        // Opcode keys are already lowercased by the tokenizer (see the
        // `resolved` build-up above), so "dysekt_zone_color" always matches
        // regardless of the source file's original casing.
        if (resolved.count ("dysekt_zone_color"))
        {
            const auto c = juce::Colour::fromString (resolved.at ("dysekt_zone_color"));
            z.hasCustomColour  = true;
            // Rebuild the packed ARGB from component accessors rather than
            // relying on a getARGB()-style internal accessor, which JUCE
            // doesn't expose publicly as a plain uint32 getter.
            z.customColourArgb = ((juce::uint32) c.getAlpha() << 24)
                                | ((juce::uint32) c.getRed()   << 16)
                                | ((juce::uint32) c.getGreen() << 8)
                                | ((juce::uint32) c.getBlue());
        }

        // Output-bus override — see SfzExporter for the write side. Same
        // lowercased-key guarantee as dysekt_zone_color above.
        z.outputBus = juce::jlimit (0, 15, intOpcode (resolved, "dysekt_output_bus", 0));

        // Manual mixer-pin override — see SfzExporter for the write side
        // and SampleZone::showInMixer for the round-trip rationale.
        z.showInMixer = resolved.count ("dysekt_show_in_mixer") != 0;

        z.group           = intOpcode (resolved, "group", 0);
        z.offBy           = intOpcode (resolved, "off_by", 0);
        z.sequencePosition = intOpcode (resolved, "seq_position", 0);
        z.sequenceLength   = intOpcode (resolved, "seq_length", 0);

        const auto sampleIt = resolved.find ("sample");
        if (sampleIt != resolved.end())
        {
            z.sampleFile = resolveSamplePath (sampleIt->second, baseDirectory, defaultPath);
            if (! z.sampleFile.existsAsFile())
                warnings.push_back ({ SfzImporter::Warning::Kind::missingSample, lineForWarnings,
                                       "Region " + juce::String (regionIndex) + ": " + sampleIt->second });
        }
        else
        {
            warnings.push_back ({ SfzImporter::Warning::Kind::malformedOpcode, lineForWarnings,
                                   "Region " + juce::String (regionIndex) + " has no sample= opcode; skipped" });
        }

        for (const auto& [k, v] : resolved)
            if (supportedOpcodes().count (k) == 0)
                z.extraOpcodes.emplace_back (k, v);

        return z;
    }
}

// =============================================================================
SfzImporter::Result SfzImporter::importFile (const juce::File& sfzFile)
{
    if (! sfzFile.existsAsFile())
    {
        Result r;
        r.errorMessage = "File not found: " + sfzFile.getFullPathName();
        return r;
    }

    auto result = importText (sfzFile.loadFileAsString(), sfzFile.getParentDirectory());
    if (result.success)
        result.instrument.name = sfzFile.getFileNameWithoutExtension();
    return result;
}

SfzImporter::Result SfzImporter::importText (const juce::String& sfzText, const juce::File& baseDirectory)
{
    Result result;

    const auto tokens = tokenize (sfzText, result.warnings);

    // Capture <curve>/<effect>/<master>/<midi> content verbatim before the
    // token-driven pass below, which only tracks their presence (for the
    // unsupportedHeader warning) and otherwise discards them — see
    // extractRawHeaderBlocks()'s comment for why this needs the raw text
    // rather than the token stream.
    result.instrument.rawExtraHeaders = extractRawHeaderBlocks (sfzText, skippedHeaders());

    OpcodeMap globalOpcodes;
    OpcodeMap groupOpcodes;
    OpcodeMap regionOpcodes;
    bool inRegion = false;
    bool skippingHeaderBody = false;   // inside <control>/<curve>/etc — ignore its opcodes
    bool inControlHeader = false;      // specifically <control> — default_path= is still read from here

    // Whether the <control> block currently being scanned contains any
    // opcode besides default_path=. The unsupportedHeader warning is only
    // worth surfacing when something was actually dropped, so this is
    // decided once the block's opcodes have been seen rather than the
    // moment <control> is opened.
    bool controlHeaderHadOtherOpcodes = false;
    int controlHeaderLine = 0;

    // default_path=, captured from <control> if present. Per spec, prepended
    // to every relative sample= from this point in the file onward (see
    // resolveSamplePath's header comment for why no other <control> opcode
    // is handled). Tracked here rather than in an OpcodeMap so it never
    // leaks into a zone's extraOpcodes.
    juce::String currentDefaultPath;

    // Which inheritance level bare (non-region) opcodes currently apply to.
    // Starts at Global so a file with no <global> header still lets stray
    // top-of-file opcodes act as defaults, matching common SFZ authoring.
    enum class Context { global, group } context = Context::global;

    int regionIndex = 0;
    int lastLine = 1;

    auto flushRegion = [&]
    {
        if (! inRegion)
            return;

        OpcodeMap resolved = globalOpcodes;
        for (const auto& [k, v] : groupOpcodes) resolved[k] = v;
        for (const auto& [k, v] : regionOpcodes) resolved[k] = v;

        ++regionIndex;
        result.instrument.zones.push_back (buildZone (resolved, baseDirectory, currentDefaultPath,
                                                        regionIndex, result.warnings, lastLine));

        regionOpcodes.clear();
        inRegion = false;
    };

    for (const auto& t : tokens)
    {
        lastLine = t.line;

        if (t.type == Token::Type::header)
        {
            flushRegion();

            if (inControlHeader && controlHeaderHadOtherOpcodes)
            {
                result.warnings.push_back ({ Warning::Kind::unsupportedHeader, controlHeaderLine,
                                              "<control> opcodes other than default_path are not imported "
                                              "in this release" });
            }
            inControlHeader = false;

            if (t.headerName == "region")
            {
                inRegion = true;
                skippingHeaderBody = false;
            }
            else if (t.headerName == "group")
            {
                groupOpcodes.clear();
                context = Context::group;
                skippingHeaderBody = false;
            }
            else if (t.headerName == "global")
            {
                globalOpcodes.clear();
                groupOpcodes.clear();
                context = Context::global;
                skippingHeaderBody = false;
            }
            else if (t.headerName == "control")
            {
                // Still a "skipped" header for every opcode except
                // default_path= (handled specially below) — see
                // resolveSamplePath's comment for why the rest of <control>
                // stays out of scope. Whether this is actually worth a
                // warning depends on what opcodes turn up inside it, so
                // the warning itself is deferred until the block closes
                // (see the flushRegion()-adjacent check above).
                skippingHeaderBody = true;
                inControlHeader = true;
                controlHeaderHadOtherOpcodes = false;
                controlHeaderLine = t.line;
            }
            else if (skippedHeaders().count (t.headerName))
            {
                skippingHeaderBody = true;
                result.warnings.push_back ({ Warning::Kind::unsupportedHeader, t.line,
                                              "<" + t.headerName + "> is not editable in this release; "
                                              "its content is preserved as-is and re-exported unchanged" });
            }
            else
            {
                skippingHeaderBody = true;
                result.warnings.push_back ({ Warning::Kind::unsupportedHeader, t.line,
                                              "Unrecognised header <" + t.headerName + ">" });
            }
            continue;
        }

        // Token::Type::opcode
        if (skippingHeaderBody)
        {
            if (inControlHeader)
            {
                if (t.opcodeKey == "default_path")
                    currentDefaultPath = t.opcodeValue;
                else
                    controlHeaderHadOtherOpcodes = true;
            }
            continue;
        }

        if (inRegion)
            applyToken (regionOpcodes, t);
        else if (context == Context::group)
            applyToken (groupOpcodes, t);
        else
            applyToken (globalOpcodes, t);
    }
    flushRegion();

    // If <control> was the last header block in the file, the deferred
    // warning above never got a following header token to trigger it.
    if (inControlHeader && controlHeaderHadOtherOpcodes)
    {
        result.warnings.push_back ({ Warning::Kind::unsupportedHeader, controlHeaderLine,
                                      "<control> opcodes other than default_path are not imported "
                                      "in this release" });
    }

    result.success = true;
    return result;
}
