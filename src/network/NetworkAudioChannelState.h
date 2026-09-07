#pragma once

#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

//==============================================================================
// NetworkAudioChannelState
//
// Realtime-safe control/state for METRO's dedicated Network Audio channel.
// UI writes atomics; the network/audio path only reads atomics.  No locks,
// allocations, or ValueTree operations are required from the audio callback.
//==============================================================================
class NetworkAudioChannelState final
{
public:
    static NetworkAudioChannelState& instance() noexcept
    {
        static NetworkAudioChannelState state;
        return state;
    }

    std::atomic<bool> enabled { true };
    std::atomic<bool> muted   { false };
    std::atomic<bool> solo    { false };
    std::atomic<float> gainDb { 0.0f };
    std::atomic<float> pan    { 0.0f };
    std::atomic<float> peakL  { 0.0f };
    std::atomic<float> peakR  { 0.0f };
    std::atomic<bool> recordArm { false };
    std::atomic<bool> monitor   { true };

    void setGainDb (float value) noexcept
    {
        gainDb.store (juce::jlimit (-100.0f, 24.0f, value), std::memory_order_relaxed);
    }

    void setPan (float value) noexcept
    {
        pan.store (juce::jlimit (-1.0f, 1.0f, value), std::memory_order_relaxed);
    }

    float linearGain() const noexcept
    {
        return juce::Decibels::decibelsToGain (gainDb.load (std::memory_order_relaxed), -100.0f);
    }

    void publishPeak (float left, float right) noexcept
    {
        peakL.store (left, std::memory_order_relaxed);
        peakR.store (right, std::memory_order_relaxed);
    }

private:
    NetworkAudioChannelState() = default;
};

inline NetworkAudioChannelState& getNetworkAudioChannelState() noexcept
{
    return NetworkAudioChannelState::instance();
}
