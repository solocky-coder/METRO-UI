#pragma once
// =============================================================================
//  SfzZoneColours.h  —  shared per-<region> colour picker for SFZ-PLAYER
//  ─────────────────────────────────────────────────────────────────────────
//  Both the ZONES view (SfzPlayerDropdownPanel::parseSfzZones) and the
//  waveform's note-slice colouring (SoundFontLoader::finishAndPost) need to
//  show the SAME colour for the same <region> — otherwise the waveform
//  strip and the ZONES list visually disagree about grouping, which is the
//  exact bug this header exists to prevent.
//
//  These two call sites run on different threads (UI thread for the ZONES
//  parse, background load-pool thread for the waveform render) and there
//  are 10+ places across the codebase that kick off an SfzPlayer2-target
//  load, so passing a live colour list between them isn't practical.
//
//  Instead, colour is a pure function of the region's index within the
//  file: sfzZoneColour(i) always returns the same colour for the same i,
//  picked from a wide 32-colour palette via a fixed hash so the sequence
//  looks scattered/arbitrary rather than a boring ROYGBIV cycle — while
//  staying perfectly reproducible. As long as both parsers walk <region>
//  blocks in the same top-to-bottom file order (they do), region i always
//  gets the same colour in both places, and reloading the same file always
//  reproduces the same colours (no shuffle-on-reload surprise).
// =============================================================================

#include <juce_graphics/juce_graphics.h>

namespace SfzZoneColours
{
    // Same 32-colour set already used as the Slicer engine's random default
    // slice palette (see Slice.h) — kept identical so SFZ-PLAYER zone
    // colours look consistent with the rest of the app's palette.
    static const juce::uint32 kPalette[32] = {
        // Reds / Oranges
        0xFFD82626, 0xFFF45F3D, 0xFFAD541E, 0xFFF28D0C,
        // Yellows / Golds
        0xFFE0BC51, 0xFFC1B60A, 0xFFC2D826, 0xFFBBF43D,
        // Greens
        0xFF66AD1E, 0xFF54F20C, 0xFF63E051, 0xFF0AC115, 0xFF26D852,
        // Teals / Cyans
        0xFF3DF48D, 0xFF1EAD77, 0xFF0CF2C7, 0xFF51E0E0, 0xFF0A9FC1,
        // Blues
        0xFF2695D8, 0xFF3D8DF4, 0xFF1E42AD, 0xFF0C1BF2,
        // Purples
        0xFF6351E0, 0xFF430AC1, 0xFF7F26D8, 0xFFBB3DF4, 0xFF9B1EAD,
        // Pinks / Magentas
        0xFFF20CE3, 0xFFE051BC, 0xFFC10A71, 0xFFD82669, 0xFFF43D5F,
    };

    /** Deterministic "looks random" colour for the region at this index
     *  (0-based, in file order). A fixed multiplicative hash scatters
     *  consecutive indices across the palette instead of walking it in
     *  order, while remaining 100% reproducible given the same index. */
    inline juce::uint32 zoneColourArgb (int regionIndex) noexcept
    {
        // 2654435761 is Knuth's multiplicative hash constant — cheap,
        // well-scattered, no need for a real PRNG here.
        const juce::uint32 h = (static_cast<juce::uint32> (regionIndex) * 2654435761u) >> 27;
        return kPalette[h % 32];
    }

    inline juce::Colour zoneColour (int regionIndex) noexcept
    {
        return juce::Colour (zoneColourArgb (regionIndex));
    }
}
