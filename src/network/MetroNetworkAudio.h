#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// METRO's network-audio layer deliberately exposes a transport-neutral API.
// AOO/SonoBus is the first backend; another secure transport can be added later
// without changing the DAW routing layer.
class MetroNetworkAudio
{
public:
    struct SourceInfo
    {
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

    // Starts the network subsystem. Network I/O is kept off the audio thread.
    bool start();
    void stop();
    bool isRunning() const noexcept;

    // SonoBus/AOO discovery session.
    bool connectToServer(const juce::String& host,
                         int port,
                         const juce::String& username,
                         const juce::String& password);
    bool joinGroup(const juce::String& group,
                   const juce::String& password = {},
                   bool isPublic = false);
    void leaveGroup(const juce::String& group);
    void disconnect();

    // Called from the audio callback. This performs only the lock-free AOO sink
    // processing; it never performs socket I/O or heap allocation.
    void process(juce::AudioBuffer<float>& destination,
                 int numSamples,
                 double sampleRate);

    std::vector<SourceInfo> getSources() const;
    void setSourceListener(SourceListener listener);

    // Preferred studio defaults: the normal use case is a local Wi-Fi LAN.
    static constexpr const char* defaultServer = "aoo.sonobus.net";
    static constexpr int defaultServerPort = 10998;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
