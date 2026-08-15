#include "SfzImporter.h"
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
            "cutoff", "resonance", "group", "off_by", "seq_position", "seq_length"
        };
        return s;
    }

    const std::set<juce::String>& skippedHeaders()
    {
        // Headers we intentionally do not import in the first release
        // (plan §5: "macros, includes, keyswitches, curves, and advanced
        // modulation can be scheduled after the core release").
        static const std::set<juce::String> s = { "control", "curve", "effect", "master", "midi" };
        return s;
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

    int64_t int64Opcode (const OpcodeMap& m, const juce::String& key, int64_t fallback)
    {
        const auto it = m.find (key);
        return it == m.end() ? fallback : (int64_t) it->second.getLargeIntValue();
    }

    juce::File resolveSamplePath (const juce::String& rawPath, const juce::File& baseDirectory)
    {
        if (rawPath.isEmpty())
            return {};

        // SFZ paths may use either slash style; normalise to the host's.
        auto normalised = rawPath.replaceCharacter ('\\', '/');

        const juce::File asAbsolute (normalised);
        if (juce::File::isAbsolutePath (normalised))
            return asAbsolute;

        if (baseDirectory == juce::File())
            return juce::File::getCurrentWorkingDirectory().getChildFile (normalised);

        return baseDirectory.getChildFile (normalised);
    }

    SampleZone buildZone (const OpcodeMap& resolved, const juce::File& baseDirectory,
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
        if (resolved.count ("key"))
        {
            const auto k = juce::jlimit (0, 127, intOpcode (resolved, "key", 60));
            z.lowKey = z.highKey = z.rootKey = k;
        }

        z.lowKey  = juce::jlimit (0, 127, intOpcode (resolved, "lokey", z.lowKey));
        z.highKey = juce::jlimit (0, 127, intOpcode (resolved, "hikey", z.highKey));
        z.rootKey = juce::jlimit (0, 127, intOpcode (resolved, "pitch_keycenter", z.rootKey));

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

        z.sampleStart = int64Opcode (resolved, "offset", 0);
        z.sampleEnd   = int64Opcode (resolved, "end", -1);
        z.loopStart   = int64Opcode (resolved, "loop_start", -1);
        z.loopEnd     = int64Opcode (resolved, "loop_end", -1);
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

        z.group           = intOpcode (resolved, "group", 0);
        z.offBy           = intOpcode (resolved, "off_by", 0);
        z.sequencePosition = intOpcode (resolved, "seq_position", 0);
        z.sequenceLength   = intOpcode (resolved, "seq_length", 0);

        const auto sampleIt = resolved.find ("sample");
        if (sampleIt != resolved.end())
        {
            z.sampleFile = resolveSamplePath (sampleIt->second, baseDirectory);
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

    OpcodeMap globalOpcodes;
    OpcodeMap groupOpcodes;
    OpcodeMap regionOpcodes;
    bool inRegion = false;
    bool skippingHeaderBody = false;   // inside <control>/<curve>/etc — ignore its opcodes

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
        result.instrument.zones.push_back (buildZone (resolved, baseDirectory, regionIndex, result.warnings, lastLine));

        regionOpcodes.clear();
        inRegion = false;
    };

    for (const auto& t : tokens)
    {
        lastLine = t.line;

        if (t.type == Token::Type::header)
        {
            flushRegion();

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
            else if (skippedHeaders().count (t.headerName))
            {
                skippingHeaderBody = true;
                result.warnings.push_back ({ Warning::Kind::unsupportedHeader, t.line,
                                              "<" + t.headerName + "> is not imported in this release" });
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
            continue;

        if (inRegion)
            applyToken (regionOpcodes, t);
        else if (context == Context::group)
            applyToken (groupOpcodes, t);
        else
            applyToken (globalOpcodes, t);
    }
    flushRegion();

    result.success = true;
    return result;
}
