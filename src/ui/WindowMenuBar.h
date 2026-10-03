#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

namespace WindowMenuBar
{
    inline void style (juce::DocumentWindow& window)
    {
        if (auto* bar = window.getMenuBarComponent())
        {
            const auto& t = getTheme();
            bar->setColour (juce::TextButton::buttonColourId,   t.header);
            bar->setColour (juce::TextButton::textColourOffId,  t.foreground);
            bar->setColour (juce::TextButton::buttonOnColourId, t.accent.withAlpha (0.35f));
            bar->setColour (juce::TextButton::textColourOnId,   t.foreground);
            bar->repaint();
        }
    }

    inline void attach (juce::DocumentWindow& window, juce::MenuBarModel* model, int height)
    {
        window.setMenuBar (model, height);
        if (model != nullptr)
            style (window);
    }
}
