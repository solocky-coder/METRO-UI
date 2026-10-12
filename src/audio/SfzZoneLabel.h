#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// Shared SFZ zone-range label used by Arranger layer rows and Mixer zone rows.
// Track/sample names remain untouched for stable identity and layer matching.
namespace SfzZoneLabel
{
    inline juce::String range (int loKey, int hiKey)
    {
        if (loKey < 0 || hiKey < loKey || hiKey > 127)
            return {};
        const auto lo = juce::MidiMessage::getMidiNoteName (loKey, true, true, 3);
        if (loKey == hiKey)
            return "[" + lo + "]";
        return "[" + lo + "-" + juce::MidiMessage::getMidiNoteName (hiKey, true, true, 3) + "]";
    }

    inline juce::String withRange (const juce::String& name, int loKey, int hiKey)
    {
        const auto label = range (loKey, hiKey);
        return label.isEmpty() ? name : name + "  " + label;
    }
}
