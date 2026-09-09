#pragma once

#include "../PluginProcessor.h"
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioRecorder.h"
#include <atomic>

// Standalone network-audio bridge. The network engine is owned by MainWindow;
// this processor consumes a METRO Audio track when one exists, otherwise it
// can render the explicitly selected NetworkSource as the migration fallback.
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

    // Create a normal METRO Audio track whose route points at one discovered
    // SonoBus/AOO source channel. Returns the new track index, or -1 when the
    // standalone processor is not currently available.
    static int createActiveNetworkAudioTrack (int64_t sourceKey,
                                              int32_t sourceId,
                                              int sourceChannel,
                                              const juce::String& sourceName,
                                              const juce::String& userName = {}) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        if (processor == nullptr)
            return -1;

        return processor->sequencer.addNetworkAudioTrack (sourceKey, sourceId,
                                                          juce::jmax (0, sourceChannel),
                                                          sourceName, userName);
    }

    // Message-thread API for the temporary migration fallback. The normal
    // Arrange/track path should populate a TrackType::Audio route instead.
    static void setActiveNetworkSource (int64_t sourceKey, int sourceChannel) noexcept
    {
        activeSourceKey.store (sourceKey, std::memory_order_release);
        activeSourceChannel.store (juce::jmax (0, sourceChannel), std::memory_order_release);
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
        networkBuffer.setSize (2, capacity, false, true, true);
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

        bool renderedTrack = false;
        const int numTracks = sequencer.getNumTracks();
        for (int trackIndex = 0; trackIndex < numTracks; ++trackIndex)
        {
            const auto info = sequencer.getTrackInfo (trackIndex);
            if (info.type != TrackType::Audio || ! info.enabled)
                continue;

            // The track owns the opaque sourceKey route identity. Read the
            // network fields directly through the dedicated route API rather
            // than relying on display-only TrackInfo fields.
            int64_t sourceKey = 0;
            int32_t sourceId = 0;
            int sourceChannel = 0;
            if (! sequencer.getNetworkAudioRoute (trackIndex, sourceKey, sourceId, sourceChannel))
                continue;
            sourceChannel = juce::jmax (0, sourceChannel);

            networkBuffer.clear();
            if (! audio->processSourceChannel (networkBuffer, numSamples, networkSampleRate,
                                               sourceKey, sourceChannel))
                continue;

            const float gain = juce::Decibels::decibelsToGain (info.volumeDb);
            const float pan = juce::jlimit (-1.0f, 1.0f, info.pan);
            const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            const float leftGain = gain * std::cos (angle);
            const float rightGain = gain * std::sin (angle);
            if (buffer.getNumChannels() > 0) buffer.addFrom (0, 0, networkBuffer, 0, 0, numSamples, leftGain);
            if (buffer.getNumChannels() > 1) buffer.addFrom (1, 0, networkBuffer, 1, 0, numSamples, rightGain);
            renderedTrack = true;
        }

        // Migration fallback: allow the settings/source list to prove the
        // engine->NetworkSource->audio path before ArrangeView track creation
        // is wired up. This is explicitly separate from the Audio-track path.
        if (! renderedTrack)
        {
            const auto sourceKey = activeSourceKey.load (std::memory_order_acquire);
            const auto sourceChannel = activeSourceChannel.load (std::memory_order_acquire);
            if (sourceKey != 0)
            {
                networkBuffer.clear();
                if (audio->processSourceChannel (networkBuffer, numSamples, networkSampleRate,
                                                 sourceKey, sourceChannel))
                {
                    if (buffer.getNumChannels() > 0) buffer.addFrom (0, 0, networkBuffer, 0, 0, numSamples);
                    if (buffer.getNumChannels() > 1) buffer.addFrom (1, 0, networkBuffer, 1, 0, numSamples);
                }
            }
        }

        if (recorder.isRecording())
            recorder.push (networkBuffer, numSamples);
    }

private:
    inline static std::atomic<MetroNetworkAudio*> activeNetworkAudio { nullptr };
    inline static std::atomic<NetworkAudioProcessor*> activeProcessor { nullptr };
    inline static std::atomic<int64_t> activeSourceKey { 0 };
    inline static std::atomic<int> activeSourceChannel { 0 };

    MetroNetworkAudio* networkAudio = nullptr;
    double networkSampleRate = 44100.0;
    juce::AudioBuffer<float> networkBuffer;
    NetworkAudioRecorder recorder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioProcessor)
};
