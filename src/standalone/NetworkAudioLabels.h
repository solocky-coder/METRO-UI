#pragma once

#include <juce_core/juce_core.h>

// Persists user-chosen names for Direct USB devices (peer IP address -> label),
// so a renamed iPad keeps its name across sessions. Stored next to the other
// standalone settings as a small JSON object.
namespace NetworkAudioLabels
{
    inline juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("DYSEKT-SF")
                   .getChildFile ("usb-device-labels.json");
    }

    inline juce::StringPairArray load()
    {
        juce::StringPairArray labels;
        const auto f = file();
        if (! f.existsAsFile())
            return labels;

        const auto parsed = juce::JSON::parse (f);
        if (auto* object = parsed.getDynamicObject())
            for (const auto& property : object->getProperties())
            {
                const auto value = property.value.toString().trim();
                if (value.isNotEmpty())
                    labels.set (property.name.toString(), value);
            }
        return labels;
    }

    inline void save (const juce::StringPairArray& labels)
    {
        auto* object = new juce::DynamicObject();
        const auto& keys = labels.getAllKeys();
        for (int i = 0; i < keys.size(); ++i)
            object->setProperty (juce::Identifier (keys[i]), labels.getValue (keys[i], {}));

        const auto f = file();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::JSON::toString (juce::var (object)));
    }
}
