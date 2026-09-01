#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
//  TransportIcons — procedural glyphs for the transport cluster (to-start,
//  back, play, stop, record, loop), replacing the old "|<" / "<<" / ">" /
//  "[]" / "REC" / "LOOP" ASCII-glyph TextButtons.
//
//  Same reasoning as ToolIcons.h: drawn as juce::Path shapes rather than
//  bundled SVG/BinaryData assets, so there is zero font/asset dependency —
//  these can never render as a missing glyph, on any platform, regardless
//  of what the embedded UI typeface does or doesn't contain (that was the
//  original bug this replaces).
//
//  Styled to match the rounded, consistent-stroke-weight look of Tabler's
//  outline icon set (ti-player-track-prev / ti-player-skip-back /
//  ti-player-play / ti-player-stop / ti-circle-filled / ti-repeat) —
//  softened corners via addRoundedPolygon() below instead of sharp-cornered
//  fills, and round line caps/joins on every stroked path.
//
//  Shared verbatim by TransportBar.h and FloatingTransportBar.h/.cpp (via
//  TransportIconButton.h) so both transport rows always show identical
//  glyphs.
//==============================================================================
namespace TransportIcons
{
    enum class Kind { ToStart, Back, Play, Stop, Record, Loop };

    namespace detail
    {
        /** Builds a filled polygon with each corner rounded off by `radius`
         *  (clamped to half the shorter adjoining edge) — the standard
         *  "corner-cut + quadratic-through-vertex" construction, used here to
         *  give filled triangles/etc. the same soft-cornered look as Tabler's
         *  outline icons instead of a sharp geometric fill. */
        inline void addRoundedPolygon (juce::Path& p, const std::vector<juce::Point<float>>& pts,
                                        float radius)
        {
            const int n = (int) pts.size();
            for (int i = 0; i < n; ++i)
            {
                const auto prev = pts[(size_t) ((i - 1 + n) % n)];
                const auto cur  = pts[(size_t) i];
                const auto next = pts[(size_t) ((i + 1) % n)];

                const auto toPrev = prev - cur;
                const auto toNext = next - cur;
                const float lenPrev = toPrev.getDistanceFromOrigin();
                const float lenNext = toNext.getDistanceFromOrigin();
                const float r = juce::jmin (radius, lenPrev * 0.5f, lenNext * 0.5f);

                const auto a = cur + toPrev * (r / lenPrev);
                const auto b = cur + toNext * (r / lenNext);

                if (i == 0) p.startNewSubPath (a);
                else        p.lineTo (a);

                p.quadraticTo (cur, b);
            }
            p.closeSubPath();
        }
    }

    /** Draws the glyph for `kind` into rectangle `b`, in colour `colour`. */
    inline void draw (juce::Graphics& g, Kind kind, juce::Rectangle<float> b, juce::Colour colour)
    {
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float s  = juce::jmin (b.getWidth(), b.getHeight());
        g.setColour (colour);

        switch (kind)
        {
            case Kind::ToStart:
            {
                // Rounded vertical bar + rounded triangle pointing left —
                // mirrors ti-player-track-prev's bar-plus-chevron shape.
                juce::Path bar;
                bar.addRoundedRectangle (cx - s * 0.34f, cy - s * 0.30f,
                                          s * 0.10f, s * 0.60f, s * 0.03f);
                g.fillPath (bar);

                juce::Path tri;
                detail::addRoundedPolygon (tri, {
                    { cx + s * 0.30f, cy - s * 0.28f },
                    { cx + s * 0.30f, cy + s * 0.28f },
                    { cx - s * 0.06f, cy }
                }, s * 0.045f);
                g.fillPath (tri);
                break;
            }
            case Kind::Back:
            {
                // Two rounded chevrons — mirrors ti-player-skip-back.
                for (float dx : { -s * 0.20f, s * 0.14f })
                {
                    juce::Path tri;
                    detail::addRoundedPolygon (tri, {
                        { cx + dx + s * 0.16f, cy - s * 0.28f },
                        { cx + dx + s * 0.16f, cy + s * 0.28f },
                        { cx + dx - s * 0.20f, cy }
                    }, s * 0.045f);
                    g.fillPath (tri);
                }
                break;
            }
            case Kind::Play:
            {
                // Single rounded triangle — mirrors ti-player-play.
                juce::Path tri;
                detail::addRoundedPolygon (tri, {
                    { cx - s * 0.26f, cy - s * 0.32f },
                    { cx - s * 0.26f, cy + s * 0.32f },
                    { cx + s * 0.36f, cy }
                }, s * 0.05f);
                g.fillPath (tri);
                break;
            }
            case Kind::Stop:
            {
                // Rounded square — mirrors ti-player-stop.
                g.fillRoundedRectangle (cx - s * 0.28f, cy - s * 0.28f,
                                        s * 0.56f, s * 0.56f, s * 0.10f);
                break;
            }
            case Kind::Record:
            {
                // Filled circle — mirrors ti-circle-filled, the universal
                // record symbol; a circle has no corners left to round.
                g.fillEllipse (cx - s * 0.30f, cy - s * 0.30f, s * 0.60f, s * 0.60f);
                break;
            }
            case Kind::Loop:
            {
                // Two opposing arcs with rounded-cap arrowheads — mirrors
                // ti-repeat's cycle/repeat convention.
                const float r = s * 0.30f;
                juce::PathStrokeType stroke (s * 0.09f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded);

                juce::Path top;
                top.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                            juce::MathConstants<float>::pi * 1.05f,
                            juce::MathConstants<float>::pi * 1.95f, true);
                g.strokePath (top, stroke);

                juce::Path bottom;
                bottom.addArc (cx - r, cy - r, r * 2.0f, r * 2.0f,
                               juce::MathConstants<float>::pi * 0.05f,
                               juce::MathConstants<float>::pi * 0.95f, true);
                g.strokePath (bottom, stroke);

                auto arrowHead = [&] (juce::Point<float> tip, float angle)
                {
                    juce::Path head;
                    detail::addRoundedPolygon (head, {
                        { 0.0f, 0.0f }, { -s * 0.16f, -s * 0.09f }, { -s * 0.16f, s * 0.09f }
                    }, s * 0.02f);
                    head.applyTransform (juce::AffineTransform::rotation (angle).translated (tip));
                    g.fillPath (head);
                };
                arrowHead ({ cx + r * std::cos (juce::MathConstants<float>::pi * 1.95f),
                             cy + r * std::sin (juce::MathConstants<float>::pi * 1.95f) },
                           juce::MathConstants<float>::pi * 1.95f);
                arrowHead ({ cx + r * std::cos (juce::MathConstants<float>::pi * 0.95f),
                             cy + r * std::sin (juce::MathConstants<float>::pi * 0.95f) },
                           juce::MathConstants<float>::pi * 0.95f);
                break;
            }
        }
    }
}
