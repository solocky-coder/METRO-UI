#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

struct AudioClip
{
    juce::String filePath;
    int64_t startTick = 0;
    int64_t lengthSamples = 0;
    double sampleRate = 0.0;
    int channels = 0;

    bool isValid() const noexcept
    {
        return filePath.isNotEmpty() && lengthSamples > 0 && sampleRate > 0.0 && channels > 0;
    }

    int64_t lengthTicks (double bpm) const noexcept
    {
        if (sampleRate <= 0.0 || bpm <= 0.0) return 0;
        return juce::roundToInt64 ((double) lengthSamples * bpm * (double) MidiClip::kPPQ / (sampleRate * 60.0));
    }

    void writeToStream (juce::MemoryOutputStream& s) const
    {
        s.writeString (filePath);
        s.writeInt64 (startTick);
        s.writeInt64 (lengthSamples);
        s.writeDouble (sampleRate);
        s.writeInt (channels);
    }

    bool readFromStream (juce::MemoryInputStream& s)
    {
        filePath = s.readString();
        startTick = s.readInt64();
        lengthSamples = s.readInt64();
        sampleRate = s.readDouble();
        channels = s.readInt();
        return isValid();
    }
};
