#pragma once
// =============================================================================
//  SfzImporter.h  —  SFZ text → MultisamplerInstrument
//  ─────────────────────────────────────────────────────────────────────────
//  A standalone tokenizing parser. Per the implementation plan (§5, "Parser
//  requirements"): this is NOT built on SfzModulePanel's existing
//  line-oriented zone scanner — that scanner is good enough for the ZONES
//  preview list but can't reliably handle multi-opcode lines, quoted paths,
//  or three-level (global/group/region) inheritance, all of which real SFZ
//  files use routinely.
//
//  Supported header/opcode subset matches the plan's §5 list exactly. Any
//  opcode outside that list is preserved verbatim on the owning zone's
//  extraOpcodes so a round trip doesn't silently drop it, and is reported
//  back to the caller as an informational (not blocking) warning.
// =============================================================================

#include "MultisamplerInstrument.h"
#include <juce_core/juce_core.h>
#include <vector>

class SfzImporter
{
public:
    struct Warning
    {
        enum class Kind
        {
            unsupportedOpcode,   ///< opcode outside the documented subset (preserved, not fatal)
            unsupportedHeader,   ///< e.g. <control>, <curve>, <effect>, keyswitch macros — skipped entirely
            missingSample,       ///< sample= pointed at a file that doesn't exist at import time
            malformedOpcode,     ///< opcode=value couldn't be parsed as its expected type
            unresolvedInclude    ///< #include directive — not supported in the first release
        };

        Kind kind;
        int lineNumber = -1;      ///< 1-based; -1 if not tied to a specific line
        juce::String detail;
    };

    struct Result
    {
        bool success = false;
        MultisamplerInstrument instrument;
        std::vector<Warning> warnings;

        /** Human-readable reason success == false, e.g. "file not found" or
            a fatal parse error. Empty on success. */
        juce::String errorMessage;
    };

    /** Parses `sfzFile` into a fresh MultisamplerInstrument. Relative sample=
        paths are resolved against sfzFile's parent directory. Never throws;
        parse problems are reported as warnings and the importer keeps going
        on a best-effort basis — SFZ files in the wild routinely carry small
        inconsistencies that shouldn't block an otherwise-good import. */
    static Result importFile (const juce::File& sfzFile);

    /** Same as importFile but parses text already in memory. `baseDirectory`
        is used to resolve relative sample= paths (pass {} to leave them
        unresolved/relative — the caller is then responsible for resolution). */
    static Result importText (const juce::String& sfzText, const juce::File& baseDirectory);

private:
    SfzImporter() = delete;
};
