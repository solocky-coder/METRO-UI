#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
//  ToolIcons  —  procedural glyphs for the Select / Draw / Erase / Split / Glue
//  editing tools.
//
//  Both PianoRollComponent's right-click Tool submenu and its toolbar buttons,
//  and ArrangeView's right-click Tool submenu, draw from this single set of
//  glyphs so the two views always show identical icons for identical tools —
//  no bundled icon assets, no duplicated drawing code to drift apart.
//==============================================================================
namespace ToolIcons
{
    /** Order intentionally matches PianoRollComponent::Tool and
     *  ArrangeView::Tool (Select, Draw, Erase, Split, Glue) so callers can
     *  simply cast their own enum's underlying int across without a mapping
     *  table. */
    enum class Kind { Select, Draw, Erase, Split, Glue };

    /** Draws the glyph for `tool` into rectangle `b`, in colour `colour`.
     *  Shared verbatim by the piano roll's toolbar buttons + tool submenu and
     *  the arranger's tool submenu. */
    inline void draw (juce::Graphics& g, Kind tool, juce::Rectangle<float> b, juce::Colour colour)
    {
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float s  = juce::jmin (b.getWidth(), b.getHeight());
        g.setColour (colour);

        switch (tool)
        {
            case Kind::Select:
            {
                // Classic arrow-cursor pointer.
                const float x = b.getX() + s * 0.20f;
                const float y = b.getY() + s * 0.08f;
                juce::Path p;
                p.startNewSubPath (x, y);
                p.lineTo (x, y + s * 0.66f);
                p.lineTo (x + s * 0.17f, y + s * 0.50f);
                p.lineTo (x + s * 0.29f, y + s * 0.76f);
                p.lineTo (x + s * 0.40f, y + s * 0.70f);
                p.lineTo (x + s * 0.28f, y + s * 0.44f);
                p.lineTo (x + s * 0.47f, y + s * 0.42f);
                p.closeSubPath();
                g.fillPath (p);
                break;
            }
            case Kind::Draw:
            {
                // Pencil, tilted, tip pointing down-left.
                juce::Path p;
                p.addRoundedRectangle (-s * 0.09f, -s * 0.40f, s * 0.18f, s * 0.60f, s * 0.03f);
                p.addTriangle (-s * 0.09f, s * 0.20f, s * 0.09f, s * 0.20f, 0.0f, s * 0.40f);
                p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.78f)
                                       .translated (cx, cy));
                g.fillPath (p);
                break;
            }
            case Kind::Erase:
            {
                // Rotated eraser block with a two-tone divider line.
                const auto t = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.2f)
                                    .translated (cx, cy);
                juce::Path body;
                body.addRoundedRectangle (-s * 0.28f, -s * 0.17f, s * 0.56f, s * 0.34f, s * 0.06f);
                body.applyTransform (t);
                g.fillPath (body);

                juce::Path divider;
                divider.addLineSegment ({ -s * 0.05f, -s * 0.17f, -s * 0.05f, s * 0.17f }, s * 0.025f);
                divider.applyTransform (t);
                g.setColour (colour.contrasting (0.5f).withAlpha (0.6f));
                g.fillPath (divider);
                break;
            }
            case Kind::Split:
            {
                // Scissors point upward. The converging blade tip is the exact
                // cursor hotspot, so the visible cut point and resulting split
                // never disagree.
                const float tipY = cy - s * 0.34f;
                const float pivotY = cy + s * 0.03f;
                g.drawLine (cx, tipY, cx - s * 0.13f, pivotY, s * 0.055f);
                g.drawLine (cx, tipY, cx + s * 0.13f, pivotY, s * 0.055f);
                g.fillEllipse (cx - s * 0.055f, pivotY - s * 0.055f, s * 0.11f, s * 0.11f);
                g.drawLine (cx - s * 0.10f, pivotY, cx - s * 0.20f, cy + s * 0.20f, s * 0.05f);
                g.drawLine (cx + s * 0.10f, pivotY, cx + s * 0.20f, cy + s * 0.20f, s * 0.05f);
                g.drawEllipse (cx - s * 0.29f, cy + s * 0.16f, s * 0.16f, s * 0.16f, s * 0.04f);
                g.drawEllipse (cx + s * 0.13f, cy + s * 0.16f, s * 0.16f, s * 0.16f, s * 0.04f);
                break;
            }
            case Kind::Glue:
            {
                // Glue drop.
                juce::Path p;
                p.startNewSubPath (cx, cy - s * 0.34f);
                p.cubicTo (cx + s * 0.27f, cy - s * 0.02f, cx + s * 0.20f, cy + s * 0.34f, cx, cy + s * 0.34f);
                p.cubicTo (cx - s * 0.20f, cy + s * 0.34f, cx - s * 0.27f, cy - s * 0.02f, cx, cy - s * 0.34f);
                p.closeSubPath();
                g.fillPath (p);
                break;
            }
        }
    }

    /** Rasterises draw() into a small Drawable, for use as the leading icon
     *  on a right-click Tool submenu item (juce::PopupMenu::Item::setImage). */
    inline std::unique_ptr<juce::Drawable> makeMenuIcon (Kind tool, juce::Colour colour)
    {
        constexpr int size = 18;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);
        draw (g, tool, juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced (1.0f), colour);

        auto d = std::make_unique<juce::DrawableImage>();
        d->setImage (img);
        return d;
    }
}
