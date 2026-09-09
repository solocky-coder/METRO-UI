#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// METRO-facing adapter over the SonoBus/AOO network engine.
// AOO source IDs are only unique within a peer, so sourceKey is the opaque
// METRO handle for (remote endpoint + AOO source id). The DAW layer should
// use sourceKey rather than sourceId alone for routing.
class MetroNetworkAudio
{
public:
    struct SourceInfo
    {
        int64_t sourceKey = 0;
        int32_t sourceId = 0;
        juce::String user;
        juce::String group;
        int channels = 0;
        double sampleRate = 0.0;
        bool online = false;
        float packetLoss = 0.0f;
    };

    using SourceListener = std::function<void()>;

    MetroNetworkAudio();
    ~MetroNetworkAudio();

    MetroNetworkAudio(const MetroNetworkAudio&) = delete;
    MetroNetworkAudio& operator=(const MetroNetworkAudio&) = delete;

    bool start();
    void stop();
    bool isRunning() const noexcept;

    bool connectToServer(const juce::String& host,
                         int port,
                         const juce::String& username,
                         const juce::String& password);
    bool joinGroup(const juce::String& group,
                   const juce::String& password = {},
                   bool isPublic = false);
    void leaveGroup(const juce::String& group);
    void disconnect();

    // Legacy monitoring mix. New METRO routing uses processSourceChannel().
    void process(juce::AudioBuffer<float>& destination,
                 int numSamples,
                 double sampleRate);

    // Render exactly one discovered SonoBus/AOO source channel.
    // sourceKey is the opaque handle returned by getSources().
    bool processSourceChannel(juce::AudioBuffer<float>& destination,
                              int numSamples,
                              double sampleRate,
                              int64_t sourceKey,
                              int sourceChannel);

    std::vector<SourceInfo> getSources() const;
    void setSourceListener(SourceListener listener);

    static constexpr const char* defaultServer = "aoo.sonobus.net";
    static constexpr int defaultServerPort = 10998;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
