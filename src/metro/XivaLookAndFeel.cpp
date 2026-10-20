#include "XivaLookAndFeel.h"
#include "XivaPaint.h"

void XivaLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                            const juce::Colour&, bool isHighlighted, bool isDown)
{
    const auto& t = getTheme();
    auto b = button.getLocalBounds().toFloat().reduced (0.5f);
    const float r = juce::jmin (t.cornerRadius, b.getHeight() * 0.5f);

    auto btnCol = button.findColour (juce::TextButton::buttonColourId);
    if ((btnCol != juce::Colour() ? btnCol : t.button).isTransparent())
        return;

    const bool toggled = button.getToggleState();

    // Per-button role colours (M/S/R, tool buttons): solid fill, rounded.
    if (button.getProperties().getWithDefault ("flatFill", false))
    {
        g.setColour (toggled ? button.findColour (juce::TextButton::buttonOnColourId) : btnCol);
        g.fillRoundedRectangle (b, r);
        return;
    }

    const bool outlineOnly = button.getProperties().getWithDefault ("flatOutline", false);
    const bool lit = toggled || isDown;

    if (! outlineOnly)
    {
        g.setColour (isHighlighted && ! lit ? t.buttonHover : t.button);
        g.fillRoundedRectangle (b, r);
    }
    if (lit)
    {
        g.setColour (t.accent.withAlpha (0.12f));
        g.fillRoundedRectangle (b, r);
    }

    juce::Path p;
    p.addRoundedRectangle (b, r);
    XivaPaint::glowStroke (g, p, lit ? t.accent : t.accent.withAlpha (isHighlighted ? 0.70f : 0.28f),
                           1.0f, lit ? t.glow : 0.0f);
}

void XivaLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    XivaPaint::panel (g, juce::Rectangle<float> (0, 0, (float) width, (float) height));
}

void XivaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float pos, float a0, float a1, juce::Slider&)
{
    const auto& t = getTheme();
    auto b = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const auto c = b.getCentre();
    const float radius = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
    const float lw = juce::jmin (3.0f, radius * 0.16f);
    const float arcR = radius - lw * 0.5f;
    const float angle = a0 + pos * (a1 - a0);

    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, a0, a1, true);
    g.setColour (t.gridLine.brighter (0.25f));
    g.strokePath (track, juce::PathStrokeType (lw, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    if (angle > a0)
    {
        juce::Path v;
        v.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, a0, angle, true);
        XivaPaint::glowStroke (g, v, t.accent, lw, t.glow);
    }

    const float s = std::sin (angle), co = -std::cos (angle);
    g.setColour (t.foreground);
    g.drawLine (c.x + s * radius * 0.45f, c.y + co * radius * 0.45f,
                c.x + s * radius * 0.72f, c.y + co * radius * 0.72f, 2.0f);
}

void XivaLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y,
                                     int width, int height, bool vertical, int thumbStart,
                                     int thumbSize, bool isOver, bool isDown)
{
    const auto& t = getTheme();
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (x, y, width, height);
    if (thumbSize <= 0)
        return;

    auto th = (vertical ? juce::Rectangle<int> (x + 2, y + thumbStart, width - 4, thumbSize)
                        : juce::Rectangle<int> (x + thumbStart, y + 2, thumbSize, height - 4)).toFloat();
    g.setColour (t.accent.withAlpha (isDown ? 0.90f : isOver ? 0.70f : 0.45f));
    g.fillRoundedRectangle (th, juce::jmin (th.getWidth(), th.getHeight()) * 0.5f);
}

void XivaLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height,
                                                juce::TextEditor& te)
{
    auto bg = te.findColour (juce::TextEditor::backgroundColourId);
    if (bg.isTransparent())
        bg = getTheme().waveformBg;
    g.setColour (bg);
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f),
                            getTheme().cornerRadius * 0.6f);
}

void XivaLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                             juce::TextEditor& te)
{
    if (te.isReadOnly())
        return;
    const auto& t = getTheme();
    const bool focused = te.hasKeyboardFocus (true);
    juce::Path p;
    p.addRoundedRectangle (juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f),
                           t.cornerRadius * 0.6f);
    XivaPaint::glowStroke (g, p, focused ? t.accent : t.separator.withAlpha (0.6f),
                           1.0f, focused ? t.glow : 0.0f);
}
