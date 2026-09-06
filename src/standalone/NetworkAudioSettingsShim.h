#pragma once

#include "NetworkAudioSettingsComponent.h"

namespace metro_network_audio_settings
{
#if DYSEKT_HAS_AOO
inline MetroNetworkAudio& sharedNetworkAudio()
{
    static MetroNetworkAudio instance;
    return instance;
}
#endif
}

namespace juce
{
class MetroNetworkAudioSettingsSelector : public ::NetworkAudioSettingsComponent
{
public:
    explicit MetroNetworkAudioSettingsSelector (juce::AudioDeviceManager& deviceManager,
                                                 int, int, int, int, bool, bool, bool, bool)
        : ::NetworkAudioSettingsComponent (
#if DYSEKT_HAS_AOO
              deviceManager, &metro_network_audio_settings::sharedNetworkAudio()
#else
              deviceManager, nullptr
#endif
          )
    {
    }

    // MainWindow currently sizes the legacy selector to 500x450. Keep the
    // existing call site intact but give the combined Audio + Network panel
    // enough room for both sections.
    void setSize (int, int) { Component::setSize (680, 690); }
};
}

// MainWindow's existing AudioDeviceSelectorComponent construction is kept as
// the stable call site; only the standalone settings implementation is routed
// to METRO's combined panel.
#define AudioDeviceSelectorComponent MetroNetworkAudioSettingsSelector
