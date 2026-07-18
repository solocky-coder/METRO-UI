#pragma once
#include "../ui/DysektLookAndFeel.h"

// Windows Metro / Fluent LookAndFeel — DYSEKT-SF Metro UI Developer
// Specification (Technical) v2, LOOKANDFEEL OVERRIDES section.
//
// Subclasses DysektLookAndFeel rather than juce::LookAndFeel_V4 directly so
// every method NOT overridden here (drawToggleButton, drawTickBox, drawLabel,
// tooltips, alert boxes, the DocumentWindow title bar, getTypefaceForFont,
// makeFont/makeMonoFont, etc.) still gets DysektLookAndFeel's existing,
// already-correct behaviour for free — Metro only needs to differ where the
// spec actually asks for a different look.
//
// This is applied via DysektEditor::setLookAndFeel() only while the active
// theme is "metro" (see PluginEditor.cpp DysektEditor::applyTheme()); at all
// other times the base DysektLookAndFeel instance is active, unchanged.
//
// Note: most of DYSEKT-SF's actual on-screen surfaces (waveform, pads, mixer,
// browser, LCDs, knob strips) are hand-painted directly in each component's
// paint(), not routed through LookAndFeel virtual calls at all — those are
// themed via getTheme().name == "metro" checks inside the components
// themselves (WaveformView.cpp, PadGridView.cpp, MixerPanel.cpp, etc.), and
// are unaffected by which LookAndFeel object is active. This class overrides
// TextButton, Slider (linear + rotary), PopupMenu, and TextEditor; ComboBox
// is inherited unchanged from DysektLookAndFeel since the two are now
// pixel-identical (flat, square corners) — see DysektLookAndFeel::drawComboBox.
class MetroLookAndFeel : public DysektLookAndFeel
{
public:
    MetroLookAndFeel() = default;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isHighlighted, bool isDown) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle, juce::Slider&) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline    (juce::Graphics&, int width, int height, juce::TextEditor&) override;
};
