#pragma once

#include "MetroNetworkAudio.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>

struct NetworkAudioInput
{
    int64_t routeId = 0;
    int64_t sourceKey = 0;
    int32_t sourceId = 0;
    int sourceChannel = 0;
    int targetTrackIndex = -1;

    juce::String sourceName;
    juce::String userName;

    std::atomic<bool> enabled { true };
    std::atomic<bool> recordArm { false };
    std::atomic<bool> monitor { true };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> pan { 0.0f };

    bool matches (int64_t key, int channel) const noexcept
    {
        return sourceKey == key && sourceChannel == channel;
    }

    bool isAssigned() const noexcept { return targetTrackIndex >= 0; }
};

struct NetworkAudioInputRoute
{
    int64_t routeId = 0;
    int64_t sourceKey = 0;
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
