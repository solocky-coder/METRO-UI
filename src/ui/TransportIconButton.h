#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "TransportIcons.h"
#include "../metro/MetroColours.h"

// ────────────────────────────────────────────────────────────────────────────
//  TransportIconButton — vector-icon replacement for the old ASCII-glyph
//  ("|<", "<<", ">", "[]", "REC", "LOOP") juce::TextButtons.
//
//  Shared verbatim by TransportBar.h and FloatingTransportBar.h/.cpp so both
//  transport rows always show identical glyphs and identical hover/active
//  feedback — the same "single source of truth" reasoning as ToolIcons.h for
//  the piano roll / arranger tool glyphs, just as a real Button rather than a
//  static draw() free function, since these need click/toggle/tooltip
//  behaviour too.
//
//  Glyphs are drawn procedurally via TransportIcons::draw() (juce::Path
//  shapes) rather than loaded from bundled SVG/BinaryData assets — zero
//  font/asset dependency, so nothing here can ever render as a missing
//  glyph on any platform.
//
//  Colour logic follows METRO's existing per-button tint convention (see the
//  old configureTransportButton() this replaces): Base::Surface off-fill,
//  tint-at-32%-alpha on-fill, tint-coloured glyph off, white glyph on — with
//  an added hover wash and a toggled-on outline so PLAY/REC/LOOP being
//  engaged is unambiguous even at this compact scale.
// ────────────────────────────────────────────────────────────────────────────
class TransportIconButton final : public juce::Button
{
public:
    TransportIconButton() : juce::Button ({}) {}

    /** Mirrors the old configureTransportButton(TextButton&, text, tint, tooltip)
     *  signature so every call site is still a one-liner — just hand it a
     *  TransportIcons::Kind instead of a text glyph. */
    void configure (TransportIcons::Kind iconKind, juce::Colour tintColour,
                     const juce::String& tooltip)
    {
        kind = iconKind;
        tint = tintColour;
        setTooltip (tooltip);
        repaint();
    }

private:
    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        using namespace dysekt::metro;

        const auto b = getLocalBounds().toFloat();
        const bool on = getToggleState();
        constexpr float corner = 3.0f;

        // ── Background ──────────────────────────────────────────────────
        juce::Colour bg = Base::Surface;
        if (on)                     bg = tint.withAlpha (0.32f);
        else if (isButtonDown)      bg = Base::Elevated;
        else if (isMouseOverButton) bg = Base::Elevated.withAlpha (0.65f);
        g.setColour (bg);
        g.fillRoundedRectangle (b, corner);

        // ── Active/hover outline — a second, unambiguous cue beyond the
        // fill alone so toggled state reads at a glance. ──────────────────
        if (on)
        {
            g.setColour (tint);
            g.drawRoundedRectangle (b.reduced (0.75f), corner, 1.5f);
        }
        else if (isMouseOverButton)
        {
            g.setColour (tint.withAlpha (0.55f));
            g.drawRoundedRectangle (b.reduced (0.75f), corner, 1.0f);
        }

        // ── Glyph ───────────────────────────────────────────────────────
        const auto iconColour = on ? Base::White
                                    : (isMouseOverButton ? tint.brighter (0.35f)
                                                          : tint.brighter (0.15f));
        // Slightly tighter inset when pressed reads as a subtle "push"
        // without needing to move the whole glyph off-centre.
        const float insetFrac = isButtonDown ? 0.24f : 0.20f;
        TransportIcons::draw (g, kind,
            b.reduced (b.getWidth() * insetFrac, b.getHeight() * insetFrac), iconColour);
    }

    TransportIcons::Kind kind = TransportIcons::Kind::Play;
    juce::Colour tint { juce::Colours::white };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportIconButton)
};
