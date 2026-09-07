#pragma once

#include "../PluginProcessor.h"
#include "../network/MetroNetworkAudio.h"

// Standalone-only bridge that puts the decoded SonoBus/AOO stream into the
// same AudioProcessor output path as METRO's normal engine.  This deliberately
// lives outside DysektProcessor so the VST3/plugin processor remains free of
// the standalone network transport dependency.
class NetworkAudioProcessor final : public DysektProcessor
{
public:
    NetworkAudioProcessor() = default;
    ~NetworkAudioProcessor() override = default;

    static void setActiveNetworkAudio (MetroNetworkAudio* audio) noexcept
    {
        activeNetworkAudio = audio;
    }

    void setNetworkAudio (MetroNetworkAudio* audio) noexcept
    {
        networkAudio = audio;
        setActiveNetworkAudio (audio);
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        DysektProcessor::prepareToPlay (sampleRate, samplesPerBlock);
        networkSampleRate = sampleRate;
        const int capacity = juce::jmax (4096, samplesPerBlock > 0 ? samplesPerBlock : 512);
        networkBuffer.setSize (64, capacity, false, true, true);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        DysektProcessor::processBlock (buffer, midi);

        auto* audio = networkAudio != nullptr ? networkAudio : activeNetworkAudio;
        if (audio == nullptr || ! audio->isRunning())
            return;

        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0 || numSamples > networkBuffer.getNumSamples())
            return;

        networkBuffer.clear (0, 0, numSamples);
        audio->process (networkBuffer, numSamples, networkSampleRate);

        const int channels = juce::jmin (2, buffer.getNumChannels(), networkBuffer.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
            buffer.addFrom (channel, 0, networkBuffer, channel, 0, numSamples);
    }

private:
    inline static std::atomic<MetroNetworkAudio*> activeNetworkAudio { nullptr };
    MetroNetworkAudio* networkAudio = nullptr;
    double networkSampleRate = 44100.0;
    juce::AudioBuffer<float> networkBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioProcessor)
};
