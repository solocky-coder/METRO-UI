#pragma once
// =============================================================================
//  DrumPadModel.h  —  zones -> 128-pad model for the drum-pads view
//  ─────────────────────────────────────────────────────────────────────────
//  Pure, UI-free logic (juce_core only) so it can be unit-tested beside
//  MultisamplerRoundTripTests.cpp. DrumPadView turns this into pixels.
//
//  One pad per MIDI note (0-127).  bank = note / 16, pad-in-bank = note % 16.
//  A zone covering several keys appears on EVERY key it covers; the zone's
//  rootKey pad is flagged `isRoot` so the view can draw it at full strength
//  and the others dimmed.  Several zones on one note (velocity layers /
//  round-robin) collapse into one pad with `layers` = how many.
// =============================================================================

#include "MultisamplerInstrument.h"
#include <juce_core/juce_core.h>
#include <array>

namespace DrumPadModel
{
    constexpr int kNumNotes    = 128;
    constexpr int kPadsPerBank = 16;
    constexpr int kNumBanks    = 8;

    struct PadInfo
    {
        int  zoneIndex = -1;      ///< index into instrument.zones of the primary zone; -1 = empty pad
        int  layers    = 0;       ///< number of enabled zones covering this note
        bool isRoot    = false;   ///< primary zone's rootKey == this note

        /** Palette slot for SfzZoneColours::zoneColour(). Counts ENABLED zones
            only, exactly like ZoneMapView::rebuildLayout(), so a zone has the
            same colour in both views. */
        int  colourIndex = 0;
        bool hasCustomColour = false;
        juce::uint32 customColourArgb = 0;

        int  bus = 0;             ///< 0 = Main, 1-15 = Aux
        juce::String name;
    };

    using Model = std::array<PadInfo, kNumNotes>;

    inline Model buildPadModel (const std::vector<SampleZone>& zones)
    {
        Model pads {};
        int colourIdx = 0;

        for (int zi = 0; zi < (int) zones.size(); ++zi)
        {
            const auto& z = zones[(size_t) zi];
            if (! z.enabled)
                continue;                       // muted zones never claim a pad

            const int thisColourIdx = colourIdx++;
            const int lo = juce::jlimit (0, 127, z.lowKey);
            const int hi = juce::jlimit (0, 127, z.highKey);
            if (hi < lo)
                continue;                       // inverted range: nothing to show

            const juce::String name = z.sampleFile != juce::File()
                                          ? z.sampleFile.getFileNameWithoutExtension()
                                          : juce::String ("(no sample)");

            for (int n = lo; n <= hi; ++n)
            {
                auto& p = pads[(size_t) n];
                const bool thisIsRoot = (n == z.rootKey);

                // First zone on a note becomes primary; a later zone only takes
                // over if it is rooted here and the current primary is not.
                if (p.layers == 0 || (thisIsRoot && ! p.isRoot))
                {
                    p.zoneIndex        = zi;
                    p.isRoot           = thisIsRoot;
                    p.colourIndex      = thisColourIdx;
                    p.hasCustomColour  = z.hasCustomColour;
                    p.customColourArgb = z.customColourArgb;
                    p.bus              = z.outputBus;
                    p.name             = name;
                }
                ++p.layers;
            }
        }
        return pads;
    }

    inline bool bankHasZones (const Model& pads, int bank)
    {
        for (int n = bank * kPadsPerBank; n < (bank + 1) * kPadsPerBank; ++n)
            if (pads[(size_t) n].layers > 0)
                return true;
        return false;
    }

    /** Bank to show when a kit loads: the bank holding note 36 (kick, GM) if
        it has zones, otherwise the lowest bank that has any, otherwise 0. */
    inline int selectInitialBank (const Model& pads)
    {
        const int kickBank = 36 / kPadsPerBank;
        if (bankHasZones (pads, kickBank))
            return kickBank;
        for (int b = 0; b < kNumBanks; ++b)
            if (bankHasZones (pads, b))
                return b;
        return 0;
    }
}
