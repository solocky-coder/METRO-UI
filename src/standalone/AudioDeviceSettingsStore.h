#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

// Persists the standalone app's global audio-device configuration (device type,
// input/output devices, sample rate, buffer size, channel setup) using JUCE's own
// AudioDeviceManager::createStateXml() / initialise(..., xml, ...) mechanism.
//
// Stored next to the other standalone settings (see NetworkAudioLabels.h). This is
// deliberately independent of project Save/Load: audio hardware settings are
// per-machine, not per-project.
//
// Only an explicit Save writes the file, so unsaved changes made in the Audio
// Settings panel never replace the last saved configuration.
namespace AudioDeviceSettingsStore
{
    inline juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("DYSEKT-SF")
                   .getChildFile ("audio-device-state.xml");
    }

    // Returns the saved <DEVICESETUP> element, or nullptr if there is none or the
    // file is missing/corrupt (caller then falls back to normal initialisation).
    inline std::unique_ptr<juce::XmlElement> load()
    {
        const auto f = file();
        if (! f.existsAsFile())
            return nullptr;

        auto xml = juce::XmlDocument::parse (f);
        if (xml == nullptr || ! xml->hasTagName ("DEVICESETUP"))
            return nullptr;

        return xml;
    }

    // Initialises the device manager from the saved configuration when there is one.
    // JUCE itself falls back to the default device if the saved device is gone; if the
    // saved state is unusable in any other way we retry with plain default init.
    // Returns JUCE's error string (empty on success).
    inline juce::String initialiseDeviceManager (juce::AudioDeviceManager& dm,
                                                 int numInputChannelsNeeded,
                                                 int numOutputChannelsNeeded)
    {
        const auto saved = load();
        auto error = dm.initialise (numInputChannelsNeeded, numOutputChannelsNeeded,
                                    saved.get(), true, {}, nullptr);

        if (error.isNotEmpty() && saved != nullptr)
        {
            DBG ("AudioDeviceSettingsStore: saved audio state failed (" << error
                 << "), falling back to default initialisation");
            error = dm.initialise (numInputChannelsNeeded, numOutputChannelsNeeded,
                                   nullptr, true, {}, nullptr);
        }

        return error;
    }

    // Writes the device manager's current state. The file is replaced atomically so a
    // crash mid-write can't leave a half-written file behind.
    inline juce::Result save (juce::AudioDeviceManager& dm)
    {
        // createStateXml() is only populated once a configuration has been chosen
        // explicitly (changed in the panel, or restored from a saved file). On a
        // pristine default setup there is nothing to persist yet.
        const auto xml = dm.createStateXml();
        if (xml == nullptr)
            return juce::Result::fail ("Nothing to save yet - change an audio setting first.");

        const auto f = file();
        if (! f.getParentDirectory().createDirectory())
            return juce::Result::fail ("Could not create the settings folder.");

        juce::TemporaryFile temp (f);
        if (! xml->writeTo (temp.getFile()))
            return juce::Result::fail ("Could not write the audio settings file.");

        if (! temp.overwriteTargetFileWithTemporary())
            return juce::Result::fail ("Could not replace the audio settings file.");

        return juce::Result::ok();
    }
}
