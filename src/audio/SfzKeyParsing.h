#pragma once
// =============================================================================
//  SfzKeyParsing.h  —  shared SFZ key-opcode value parser (numeric or note-name)
//  ─────────────────────────────────────────────────────────────────────────
//  The SFZ `key=`/`lokey=`/`hikey=`/`pitch_keycenter=` opcodes accept EITHER
//  a raw MIDI note number (0-127) OR a note name like "a1", "c#3", "db-1".
//  Both the ZONES view (SfzPlayerDropdownPanel::parseSfzZones) and the
//  MULTISAMPLER importer (SfzImporter::buildZone) need to parse the same
//  opcodes the same way — this used to be a lambda duplicated (with a
//  case-sensitivity gap in one copy) inside SfzPlayerDropdownPanel.cpp only,
//  which meant MULTISAMPLER silently mis-imported any file using note names
//  (a plain getIntValue() on "c3" returns 0, not 60). Extracted here so
//  there is exactly one implementation for both call sites to drift from.
//
//  DYSEKT OCTAVE CONVENTION — DELIBERATE, NOT A BUG:
//  This uses C3 == MIDI 60, matching KeysPanel.cpp / SliceLcdDisplay.cpp /
//  SliceControlBar.cpp / SfzLcdDisplay.cpp elsewhere in this app. That is
//  NOT the SFZ file-format spec's own convention (spec: MIDI note 0 = C-1,
//  so C4 == 60) — but every other view of an imported instrument in this
//  app already assumes C3 == 60, and disagreeing with them would make
//  MULTISAMPLER and ZONES show two different key mappings for the same
//  file, which is worse than disagreeing with a third-party spec value
//  nobody in-app ever compares against directly. Do not "fix" this back to
//  the spec convention without updating every other reader in the app to
//  match — see the implementation log discussion this header came out of.
// =============================================================================

#include <juce_core/juce_core.h>

namespace SfzKeyParsing
{
    /** Parses an SFZ key-opcode value that may be either a raw MIDI number
        ("60") or a note name ("c3", "C#3", "db-1", octave may be negative).
        Returns -1 if `raw` is empty or isn't recognisable as either form —
        callers should treat -1 as "couldn't parse" and fall back to a
        sensible default rather than silently using 0, which is itself a
        valid low MIDI note and not a safe stand-in for "unknown". */
    inline int parseKeyValue (const juce::String& raw) noexcept
    {
        if (raw.isEmpty())
            return -1;

        if (raw.containsOnly ("0123456789"))
            return juce::jlimit (0, 127, raw.getIntValue());

        // Note-name form: <letter>[#|b]<octave>, octave may be negative.
        // Case-insensitive throughout — SfzImporter's opcode values keep
        // their original file case (unlike ZONES' pre-lowercased scan), so
        // this must not assume the caller already lowercased `raw`.
        int i = 0;
        const auto n = raw.length();
        if (n == 0)
            return -1;

        const auto letter = juce::CharacterFunctions::toLowerCase (raw[i]);
        int semitone;
        switch (letter)
        {
            case 'c': semitone = 0;  break;
            case 'd': semitone = 2;  break;
            case 'e': semitone = 4;  break;
            case 'f': semitone = 5;  break;
            case 'g': semitone = 7;  break;
            case 'a': semitone = 9;  break;
            case 'b': semitone = 11; break;
            default:  return -1;   // not a recognised note letter
        }
        ++i;

        if (i < n)
        {
            const auto accidental = juce::CharacterFunctions::toLowerCase (raw[i]);
            if (accidental == '#')      { ++semitone; ++i; }
            else if (accidental == 'b') { --semitone; ++i; }
        }

        const auto octaveStr = raw.substring (i);
        if (octaveStr.isEmpty()
            || ! (octaveStr.containsOnly ("0123456789")
                  || (octaveStr[0] == '-' && octaveStr.substring (1).containsOnly ("0123456789"))))
            return -1;

        const int octave = octaveStr.getIntValue();
        // DYSEKT convention: C3 == MIDI note 60 (octave = note/12 - 2) — see
        // the file-level comment above before changing this offset.
        return juce::jlimit (0, 127, (octave + 2) * 12 + semitone);
    }
}
