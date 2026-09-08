#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class MetroNetworkAudio
{
public:
    struct SourceInfo
    {
        // Unique METRO identity for one remote endpoint + AOO source id.
        // AOO source ids are not globally unique across peers.
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

    bool connectToServer(const juce::String& host, int port,
                         const juce::String& username, const juce::String& password);
    bool joinGroup(const juce::String& group, const juce::String& password = {},
                   bool isPublic = false);
    void leaveGroup(const juce::String& group);
    void disconnect();

    void process(juce::AudioBuffer<float>& destination, int numSamples, double sampleRate);

    // sourceKey is the preferred routing identity. The AOO-id helper remains
    // for callers that only know the raw AOO source id.
    bool processSourceChannel(juce::AudioBuffer<float>& destination, int numSamples,
                              double sampleRate, int64_t sourceKey, int sourceChannel);
    bool processSourceChannelByAooId(juce::AudioBuffer<float>& destination, int numSamples,
                                     double sampleRate, int32_t sourceId, int sourceChannel);

    std::vector<SourceInfo> getSources() const;
    void setSourceListener(SourceListener listener);

    static constexpr const char* defaultServer = "aoo.sonobus.net";
    static constexpr int defaultServerPort = 10998;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
