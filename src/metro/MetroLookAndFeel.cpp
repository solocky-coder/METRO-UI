#include "MetroLookAndFeel.h"

// ── Buttons ──────────────────────────────────────────────────────────────────
// Flat buttons. Hover: lighter panel. Pressed: accent fill. Disabled: reduced
// opacity (handled upstream by JUCE's normal alpha-on-disabled behaviour).
void MetroLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                              const juce::Colour& /*bgColour*/,
                                              bool isHighlighted, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);

    auto btnCol = button.findColour (juce::TextButton::buttonColourId);
    auto baseBg = (btnCol != juce::Colour()) ? btnCol : getTheme().button;
    if (baseBg.isTransparent())
        return;

    const bool toggled = button.getToggleState();
    const float r = 0.0f; // manual: flat, square corners — no radius

    auto fill = isDown        ? getTheme().accent
              : toggled       ? getTheme().accent
              : isHighlighted ? getTheme().buttonHover
                              : getTheme().button;
    g.setColour (fill);
    g.fillRoundedRectangle (bounds, r);
}

// ── Combo boxes ──────────────────────────────────────────────────────────────
// Flat fill, thin border, accent only when focused — no bevel/glow.
void MetroLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                      int buttonX, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                      juce::ComboBox& box)
{
    const auto& t = getTheme();
    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    const float r = 0.0f; // manual: flat, square corners — no radius

    g.setColour (isButtonDown ? t.buttonHover : t.button);
    g.fillRoundedRectangle (bounds, r);

    const bool focused = box.hasKeyboardFocus (false);
    g.setColour (focused ? t.accent : t.separator);
    g.drawRoundedRectangle (bounds, r, 1.0f);

    const int arrowCX = buttonX + (width - buttonX) / 2;
    const int arrowCY = height / 2;
    const int arrowHalf = 4;
    g.setColour (t.foreground.withAlpha (0.85f));
    g.drawLine ((float) (arrowCX - arrowHalf), (float) (arrowCY - 2),
                (float) arrowCX,               (float) (arrowCY + 2), 1.5f);
    g.drawLine ((float) arrowCX,               (float) (arrowCY + 2),
                (float) (arrowCX + arrowHalf), (float) (arrowCY - 2), 1.5f);
}

// ── Popup menus ──────────────────────────────────────────────────────────────
// Flat card, square corners per manual's corner-radius rule (base class uses 3px).
void MetroLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const auto& t = getTheme();
    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (t.button);
    g.fillRoundedRectangle (bounds, 0.0f);
    g.setColour (t.separator);
    g.drawRoundedRectangle (bounds, 0.0f, 1.0f);
}

// ── Sliders ──────────────────────────────────────────────────────────────────
// Thin tracks. Accent-colored value. Flat round thumb, no glow.
void MetroLookAndFeel::drawLinearSlider (juce::Graphics& g, int /*x*/, int /*y*/, int width, int height,
                                          float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                          const juce::Slider::SliderStyle /*style*/, juce::Slider& slider)
{
    const auto& t = getTheme();
    auto bounds = slider.getLocalBounds().toFloat();

    const float trackH = 3.0f;
    auto track = bounds.withSizeKeepingCentre ((float) width, trackH);
    g.setColour (t.gridLine);
    g.fillRect (track);

    const float fillLeft  = track.getX();
    const float fillRight = juce::jlimit (fillLeft, track.getRight(), sliderPos);
    if (fillRight > fillLeft)
    {
        g.setColour (t.accent);
        g.fillRect (juce::Rectangle<float> (fillLeft, track.getY(), fillRight - fillLeft, track.getHeight()));
    }

    const float thumbD = juce::jmin ((float) height, 14.0f);
    auto thumb = juce::Rectangle<float> (thumbD, thumbD).withCentre ({ sliderPos, bounds.getCentreY() });
    g.setColour (t.accent);
    g.fillEllipse (thumb);
}

// ── Knobs ────────────────────────────────────────────────────────────────────
// Circular Metro knobs with a simple indicator line. No metallic textures.
// (Only reached if a real rotary juce::Slider is instantiated — DYSEKT's
// visible knob strips are hand-painted cells in SliceControlBar, themed
// separately via getTheme() there.)
void MetroLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPosProportional, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& /*slider*/)
{
    const auto& t = getTheme();
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float angle  = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const float lineW = juce::jmin (4.0f, radius * 0.22f);
    const float arcR  = radius - lineW * 0.5f;

    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (t.gridLine);
    g.strokePath (trackArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (angle > rotaryStartAngle)
    {
        juce::Path fillArc;
        fillArc.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, angle, true);
        g.setColour (t.accent);
        g.strokePath (fillArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Point<float> tip (centre.x + std::sin (angle) * radius, centre.y - std::cos (angle) * radius);
    g.setColour (t.accent);
    g.drawLine (centre.x, centre.y, tip.x, tip.y, 2.0f);
}

// ── LCD / text-editor panels ─────────────────────────────────────────────────
// Dark cards with subtle border. No glowing effects.
void MetroLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    auto bg = te.findColour (juce::TextEditor::backgroundColourId);
    if (bg.isTransparent())
        bg = getTheme().waveformBg;

    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 0.0f);
}

void MetroLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    if (te.isReadOnly())
        return;

    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    const bool focused = te.hasKeyboardFocus (true);
    g.setColour (focused ? getTheme().accent : getTheme().separator.withAlpha (0.60f));
    g.drawRoundedRectangle (bounds, 0.0f, focused ? 1.4f : 1.0f);
}
