#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

// METRO-facing adapter over the SonoBus/AOO network engine.
// AOO source IDs are only unique within a peer, so sourceKey is the opaque
// METRO handle for (remote endpoint + AOO source id). The DAW layer should
// use sourceKey rather than sourceId alone for routing.
class MetroNetworkAudio
{
public:
    // Route channel selector. Values >= 0 select one AOO channel (mono, sent
    // to both sides). kStereoPair selects AOO channels 1+2 as one stereo pair.
    // The value is stored in the same int as a single-channel route, so the
    // project stream format does not change and old projects load unchanged.
    static constexpr int kStereoPair = 1000;
    static constexpr bool isStereoRoute (int sourceChannel) noexcept { return sourceChannel == kStereoPair; }

    // What a source is doing right now, derived once a second on the io thread.
    enum class LinkState
    {
        Unknown,           // no data yet
        WaitingForFormat,  // invited, remote format not received
        ReadyNoTrack,      // format negotiated, no METRO track is reading it
        Buffering,         // a track reads it, but the sink has decoded nothing yet
        Receiving,         // audio arriving with signal
        Silent,            // audio arriving but only silence for a couple of seconds
        NoData             // a track reads it, but no packets are arriving
    };

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

        // Live status for the UI (updated about once a second).
        LinkState linkState = LinkState::Unknown;
        float peakLevel = 0.0f;       // linear peak delivered to tracks over the last second
        int rxPacketsPerSec = 0;      // UDP packets routed to this source over the last second

        // Direct USB: packets from a second UDP port on the same peer address
        // (a second SonoBus socket or instance). They are dropped, never mixed
        // into the active stream, and reported here so the UI can flag them.
        int strayPacketsPerSec = 0;
        int strayPort = 0;            // most recent stray source port, 0 if none

        // Direct USB only: the peer's IP address (e.g. "192.168.99.2"). This is
        // the stable key for a user-chosen device label; empty for group sources.
        juce::String peerAddress;
    };

    using SourceListener = std::function<void()>;

    MetroNetworkAudio();
    ~MetroNetworkAudio();

    MetroNetworkAudio(const MetroNetworkAudio&) = delete;
    MetroNetworkAudio& operator=(const MetroNetworkAudio&) = delete;

    bool start();
    bool startDirect(int localPort = 9000);
    void stop();
    bool isRunning() const noexcept;
    bool isDirectMode() const noexcept;

    // Direct USB device labels. SonoBus sends no device name over a direct
    // link, so each source defaults to "USB 1".."USB 4" (by subnet) and the
    // user can rename it. Overrides are keyed by peer IP address; an empty
    // label restores the default. Safe to call from the message thread.
    static juce::String defaultSourceLabel (const juce::String& peerAddress);
    void setSourceLabel (const juce::String& peerAddress, const juce::String& label);
    void setSourceLabels (const juce::StringPairArray& labelsByAddress);
    juce::StringPairArray getSourceLabels() const;

    bool connectToServer(const juce::String& host,
                         int port,
                         const juce::String& username,
                         const juce::String& password);
    bool connectDirectPeer(const juce::String& peerHost, int peerPort, int sourceId = 0);
    void disconnectDirectPeers();

    // Display metadata for live Direct USB peers is supplied by the
    // standalone Apple USB transport. No device discovery happens here.
    void setDirectPeerDisplayName(const juce::String& peerHost, const juce::String& displayName);
    void resetDirectPeerDisplayNames();

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

    // Diagnostics: called once per host audio callback (audio thread, lock-free)
    // with the callback's block size and how long the whole processBlock took.
    void noteHostBlock(int numSamples, int64_t elapsedMicros, float outputPeak = 0.0f) noexcept;

    std::vector<SourceInfo> getSources() const;
    void setSourceListener(SourceListener listener);

    static constexpr const char* defaultServer = "aoo.sonobus.net";
    static constexpr int defaultServerPort = 10998;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
