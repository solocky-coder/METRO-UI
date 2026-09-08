#pragma once

#include "MetroNetworkAudio.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>

//==============================================================================
// NetworkAudioInput
//
// A first-class, transport-neutral description of one network-audio input.
// The important distinction from MetroNetworkAudio is that this object names
// exactly one source/channel and one DAW track target.  It deliberately does
// not own sockets or AOO objects: transport remains in MetroNetworkAudio.
//
// Audio-thread rule:
//   - routing parameters are atomics;
//   - no String allocation or UI calls are performed by the audio path;
//   - source discovery/format changes stay on the network/message side.
//==============================================================================
struct NetworkAudioInput
{
    int64_t routeId = 0;
    int32_t sourceId = 0;
    int sourceChannel = 0;          // zero-based channel inside the remote source
    int targetTrackIndex = -1;      // SequencerEngine track index; -1 = unassigned

    juce::String sourceName;
    juce::String userName;

    std::atomic<bool> enabled { true };
    std::atomic<bool> recordArm { false };
    std::atomic<bool> monitor { true };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> pan { 0.0f };

    bool matches (int32_t id, int channel) const noexcept
    {
        return sourceId == id && sourceChannel == channel;
    }

    bool isAssigned() const noexcept
    {
        return targetTrackIndex >= 0;
    }
};

//==============================================================================
// NetworkAudioInputRoute
//
// Small POD snapshot used by the DAW routing layer.  It is intentionally
// independent of AOO so the eventual track/mixer implementation can consume
// network, hardware, or another transport through the same contract.
//==============================================================================
struct NetworkAudioInputRoute
{
    int64_t routeId = 0;
    int32_t sourceId = 0;
    int sourceChannel = 0;
    int targetTrackIndex = -1;
    bool enabled = true;
    bool recordArm = false;
    bool monitor = true;
    bool mute = false;
    bool solo = false;
    float gainDb = 0.0f;
    float pan = 0.0f;
};
