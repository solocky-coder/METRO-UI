#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/// A transparent, click-intercepting overlay shown only while the Theme
/// Editor's PICK button is active. Sits on top of a real UI (the main
/// plugin editor, or — in standalone builds — the separate SlotWindow that
/// hosts the Mixer/EQ/Arranger) and lets the user click any tagged widget
/// to select the matching row in the Theme Editor's list, instead of only
/// the small preview strip.
///
/// The host is responsible for: adding this as a child covering the full
/// area to be pickable, keeping its bounds in sync on resize, showing/
/// hiding it in response to ThemeEditorPanel::onPickModeChanged, and
/// wiring onPick to do the actual hit-testing (see
/// PluginEditor::resolveThemeKeyAt / SlotWindowContent::resolveThemeKeyAt
/// for the two existing implementations of that pattern).
class ThemePickOverlay : public juce::Component
{
public:
    std::function<void (juce::Point<int>)> onPick;

    ThemePickOverlay() { setMouseCursor (juce::MouseCursor::CrosshairCursor); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onPick) onPick (e.getPosition());
    }
};
