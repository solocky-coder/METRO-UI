#pragma once

#include "../PluginProcessor.h"
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioRecorder.h"

class NetworkAudioProcessor final : public DysektProcessor
{
public:
    NetworkAudioProcessor() { activeProcessor.store (this, std::memory_order_release); }

    ~NetworkAudioProcessor() override
    {
        stopNetworkRecording();
        if (activeProcessor.load (std::memory_order_acquire) == this)
            activeProcessor.store (nullptr, std::memory_order_release);
    }

    static void setActiveNetworkAudio (MetroNetworkAudio* audio) noexcept
    {
        activeNetworkAudio.store (audio, std::memory_order_release);
    }

    static bool startActiveNetworkRecording (const juce::File& file, int channels = 2) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        return processor != nullptr && processor->startNetworkRecording (file, channels);
    }

    static void stopActiveNetworkRecording() noexcept
    {
        if (auto* processor = activeProcessor.load (std::memory_order_acquire))
            processor->stopNetworkRecording();
    }

    static bool isActiveNetworkRecording() noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        return processor != nullptr && processor->recorder.isRecording();
    }

    void setNetworkAudio (MetroNetworkAudio* audio) noexcept
    {
        networkAudio = audio;
        setActiveNetworkAudio (audio);
    }

    bool startNetworkRecording (const juce::File& file, int channels = 2) noexcept
    {
        const double rate = networkSampleRate > 0.0 ? networkSampleRate : 44100.0;
        return recorder.start (file, rate, juce::jlimit (1, 64, channels));
    }

    void stopNetworkRecording() noexcept { recorder.stop(); }

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

        MetroNetworkAudio* audio = networkAudio;
        if (audio == nullptr)
            audio = activeNetworkAudio.load (std::memory_order_acquire);

        if (audio == nullptr || ! audio->isRunning())
            return;

        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0 || numSamples > networkBuffer.getNumSamples())
            return;

        networkBuffer.clear (0, 0, numSamples);
        audio->process (networkBuffer, numSamples, networkSampleRate);

        if (recorder.isRecording())
            recorder.push (networkBuffer, numSamples);

        const int channels = juce::jmin (2, buffer.getNumChannels(), networkBuffer.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
            buffer.addFrom (channel, 0, networkBuffer, channel, 0, numSamples);
    }

private:
    inline static std::atomic<MetroNetworkAudio*> activeNetworkAudio { nullptr };
    inline static std::atomic<NetworkAudioProcessor*> activeProcessor { nullptr };
    MetroNetworkAudio* networkAudio = nullptr;
    double networkSampleRate = 44100.0;
    juce::AudioBuffer<float> networkBuffer;
    NetworkAudioRecorder recorder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioProcessor)
};
