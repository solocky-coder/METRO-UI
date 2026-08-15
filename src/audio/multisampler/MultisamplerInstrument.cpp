#include "MultisamplerInstrument.h"
#include <algorithm>
#include <map>

SampleZone& MultisamplerInstrument::addZone (SampleZone zone)
{
    if (zone.id.isNull())
        zone.id = juce::Uuid();
    zones.push_back (std::move (zone));
    return zones.back();
}

bool MultisamplerInstrument::removeZone (const juce::Uuid& id)
{
    const auto before = zones.size();
    zones.erase (std::remove_if (zones.begin(), zones.end(),
                                  [&] (const SampleZone& z) { return z.id == id; }),
                 zones.end());
    return zones.size() != before;
}

SampleZone* MultisamplerInstrument::findZone (const juce::Uuid& id)
{
    for (auto& z : zones)
        if (z.id == id)
            return &z;
    return nullptr;
}

const SampleZone* MultisamplerInstrument::findZone (const juce::Uuid& id) const
{
    for (auto& z : zones)
        if (z.id == id)
            return &z;
    return nullptr;
}

SampleZone* MultisamplerInstrument::duplicateZone (const juce::Uuid& id)
{
    const auto it = std::find_if (zones.begin(), zones.end(),
                                   [&] (const SampleZone& z) { return z.id == id; });
    if (it == zones.end())
        return nullptr;

    SampleZone copy = *it;
    copy.id = juce::Uuid();

    const auto insertPos = std::next (it);
    const auto inserted  = zones.insert (insertPos, std::move (copy));
    return &(*inserted);
}

std::vector<const SampleZone*> MultisamplerInstrument::zonesMatching (int midiNote, int velocity) const
{
    std::vector<const SampleZone*> result;
    for (const auto& z : zones)
        if (z.matches (midiNote, velocity))
            result.push_back (&z);
    return result;
}

std::vector<std::pair<size_t, size_t>> MultisamplerInstrument::findOverlappingPairs() const
{
    std::vector<std::pair<size_t, size_t>> pairs;
    for (size_t a = 0; a < zones.size(); ++a)
    {
        for (size_t b = a + 1; b < zones.size(); ++b)
        {
            const auto& za = zones[a];
            const auto& zb = zones[b];

            const bool keysOverlap = za.lowKey <= zb.highKey && zb.lowKey <= za.highKey;
            const bool velsOverlap = za.lowVelocity <= zb.highVelocity && zb.lowVelocity <= za.highVelocity;

            if (keysOverlap && velsOverlap)
                pairs.emplace_back (a, b);
        }
    }
    return pairs;
}

std::vector<const SampleZone*> MultisamplerInstrument::missingSampleZones() const
{
    std::vector<const SampleZone*> result;
    for (const auto& z : zones)
        if (z.hasMissingSample())
            result.push_back (&z);
    return result;
}

std::vector<MultisamplerInstrument::ValidationIssue> MultisamplerInstrument::validate() const
{
    std::vector<ValidationIssue> issues;

    if (zones.empty())
    {
        issues.push_back ({ ValidationIssue::Severity::warning, juce::Uuid::null(),
                             "Instrument has no zones." });
    }

    for (const auto& z : zones)
    {
        if (z.hasMissingSample())
            issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                 "Missing sample: " + z.sampleFile.getFullPathName() });

        if (z.lowKey > z.highKey)
            issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                 "Low key is above high key." });

        if (z.lowVelocity > z.highVelocity)
            issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                 "Low velocity is above high velocity." });

        if (z.rootKey < z.lowKey || z.rootKey > z.highKey)
            issues.push_back ({ ValidationIssue::Severity::warning, z.id,
                                 "Root key falls outside this zone's key range." });

        if (z.loopMode != LoopMode::noLoop)
        {
            if (z.loopStart < 0 || z.loopEnd < 0)
                issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                     "Loop is enabled but loop points are unset." });
            else if (z.loopEnd <= z.loopStart)
                issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                     "Loop end must be after loop start." });
        }

        if (z.sequenceLength > 0 && (z.sequencePosition < 1 || z.sequencePosition > z.sequenceLength))
            issues.push_back ({ ValidationIssue::Severity::error, z.id,
                                 "Round-robin position is outside 1.." + juce::String (z.sequenceLength) + "." });
    }

    return issues;
}

bool MultisamplerInstrument::relinkSample (const juce::Uuid& id, const juce::File& newFile)
{
    if (auto* z = findZone (id))
    {
        z->sampleFile = newFile;
        return true;
    }
    return false;
}

int MultisamplerInstrument::relinkAllFromFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return 0;

    // Build a case-insensitive filename -> file lookup once, rather than
    // re-walking the folder for every missing zone.
    std::map<juce::String, juce::File> byName;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        byName[entry.getFile().getFileName().toLowerCase()] = entry.getFile();

    int relinkedCount = 0;
    for (auto& z : zones)
    {
        if (! z.hasMissingSample())
            continue;

        const auto key = z.sampleFile.getFileName().toLowerCase();
        const auto it  = byName.find (key);
        if (it != byName.end())
        {
            z.sampleFile = it->second;
            ++relinkedCount;
        }
    }
    return relinkedCount;
}
