#pragma once
#include "../ui/KeysPanel.h"
#include <vector>
#include <algorithm>

//==============================================================================
//  SfzLayoutClassifier
//
//  Heuristic: given the zones SfzPlayerDropdownPanel::parseSfzZones() already
//  extracts from a .sfz file (per-<region> loKey/hiKey + bare sample name,
//  no new file parsing needed), decide whether the file reads as a
//  percussive/drum-kit map (each key = one distinct one-shot instrument,
//  narrow disjoint ranges, GM-style drum names) or a melodic instrument
//  (one sample pitch-shifted across a wide contiguous range, often layered
//  with other regions covering the same range at different velocities).
//
//  This directly informs SFZ-PLAYER output routing: a drum kit benefits from
//  each hit going to its own outputBus (separate compression/reverb sends
//  per drum, same reasoning as the Slicer's per-slice OUT); a melodic
//  instrument is correctly served by the single summary mixer row it already
//  gets, since every zone is really "the same voice."
//
//  Two independent, corroborating signals — neither is reliable alone
//  (a one-shot melodic pluck library can have narrow ranges too; a layered
//  drum kit can have overlapping ranges), which is why this returns a
//  confidence score rather than a hard yes/no. Callers should treat this as
//  a suggestion to confirm with the user, not an instruction to silently act
//  on.
//==============================================================================
struct SfzLayoutClassification
{
    bool  isPercussive { false };
    float confidence   { 0.0f };   // 0..1 heuristic score
    int   numZones     { 0 };
};

class SfzLayoutClassifier
{
public:
    static SfzLayoutClassification classify (const std::vector<KeysPanel::Keyzone>& zones)
    {
        SfzLayoutClassification result;
        result.numZones = (int) zones.size();

        // Need at least 2 zones for "multiple outputs" to mean anything.
        if (zones.size() < 2)
            return result;

        // ── Signal 1: sample-name keyword match against GM-style drum names ──
        static const char* kDrumKeywords[] = {
            "kick", "kd", "bd", "snare", "sn", "rim", "clap", "hat", "hh",
            "hihat", "ohat", "chat", "openhat", "closedhat", "tom", "crash",
            "ride", "cymbal", "perc", "cow", "bell", "shak", "tamb", "conga",
            "bongo", "tabla", "clave", "block", "triangle", "timbale",
            "djembe", "cabasa", "guiro", "kit", "drum"
        };
        int keywordMatches = 0;
        for (const auto& z : zones)
        {
            const auto lower = z.name.toLowerCase();
            for (auto* kw : kDrumKeywords)
            {
                if (lower.contains (kw))
                {
                    ++keywordMatches;
                    break;
                }
            }
        }
        const float keywordScore = (float) keywordMatches / (float) zones.size();

        // ── Signal 2: key-range shape ─────────────────────────────────────────
        // Narrow ranges (1-3 keys) suggest one sample = one instrument, not
        // one sample pitch-shifted across the keyboard.
        int narrowCount = 0;
        for (const auto& z : zones)
        {
            const int width = z.hiKey - z.loKey + 1;
            if (width <= 3)
                ++narrowCount;
        }
        const float narrowFraction = (float) narrowCount / (float) zones.size();

        // Disjointness: sort by loKey and count how many adjacent pairs
        // overlap. Drum maps are mostly disjoint (kick ≠ snare's key); a
        // layered melodic instrument re-covers the same range repeatedly.
        std::vector<KeysPanel::Keyzone> sorted (zones.begin(), zones.end());
        std::sort (sorted.begin(), sorted.end(),
                   [] (const KeysPanel::Keyzone& a, const KeysPanel::Keyzone& b)
                   { return a.loKey < b.loKey; });

        int overlapPairs = 0;
        for (size_t i = 1; i < sorted.size(); ++i)
            if (sorted[i].loKey <= sorted[i - 1].hiKey)
                ++overlapPairs;

        const float overlapFraction  = (float) overlapPairs / (float) (sorted.size() - 1);
        const float disjointFraction = 1.0f - overlapFraction;

        // ── Combine ────────────────────────────────────────────────────────────
        const float score = 0.40f * keywordScore
                           + 0.35f * narrowFraction
                           + 0.25f * disjointFraction;

        result.confidence  = score;
        result.isPercussive = score > 0.5f;
        return result;
    }
};
