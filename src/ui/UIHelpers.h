#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ThemeData.h"
#include "../audio/SfzPlayer.h"
#include <cmath>
#include <vector>

namespace UIHelpers
{

// ── MIDI note number -> note name ───────────────────────────────────────────
// App-wide convention: C3 == MIDI note 60 (octave = note/12 - 2). This was
// previously reimplemented independently in at least 9 places across the UI
// (AddZoneOverlay, AddZonePanel, KeysPanel x2, PadGridView, Sf2Instrument-
// Workspace, SfzLcdDisplay, SfzPlayerDropdownPanel, SliceControlBar x2,
// SliceLcdDisplay) — each a hand-copied `note/12 - 2` + note-name-array pair,
// kept in sync only by comments pointing at each other. That's exactly how
// AddZoneOverlay ended up shipping with the octave off by one for a while:
// one copy drifted to `note/12 - 1` and nothing caught it at compile time.
// Single source of truth now; every call site should use this instead of
// its own copy.
inline juce::String midiNoteToName (int note)
{
    static const char* kNames[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    return juce::String (kNames[((note % 12) + 12) % 12]) + juce::String (note / 12 - 2);
}

// ── Shared SF2 track colour ─────────────────────────────────────────────────
// A given preset (bank/preset pair) always maps to the same colour, and that
// colour is what the Arranger uses for the track name + MIDI part once the
// preset is assigned to a channel (see PluginEditor.cpp's
// onPresetChannelAssigned -> pianoRollPanel.addOrUpdateSfPresetTrack). Both
// Sf2InstrumentWorkspace's preset list and Sf2ProgramGrid's preset grid pull
// from this single definition so an assigned preset always reads as the same
// colour everywhere it appears — grid cell, workspace row, and Arranger track
// alike. Presets that are merely being previewed (not assigned to a channel)
// are NOT an Arranger object yet, so callers should keep those on the theme's
// generic accent colour instead of calling this.
inline juce::Colour sf2TrackColourForPreset (const Sf2PresetInfo& preset)
{
    static const juce::Colour kPalette[] = {
        juce::Colour (0xFF4060A0), juce::Colour (0xFF60A040),
        juce::Colour (0xFFA04060), juce::Colour (0xFF40A0A0),
        juce::Colour (0xFFA0A040), juce::Colour (0xFF8060C0),
    };

    const int colourIndex = (preset.bank * 128 + preset.preset) % (int) (sizeof (kPalette) / sizeof (kPalette[0]));
    return kPalette[juce::jmax (0, colourIndex)];
}

// ── Background texture pass ─────────────────────────────────────────────────
// Shared "not a flat box" panel treatment: base gradient + cached grain +
// vignette, with a per-zone signature layer on top. See drawTexturedPanel().
enum class PanelZone
{
    Chassis,     // control strip / instrument-display frame -> brushed-metal grain
    Instrument,  // the LCD/CRT surface itself -> scanlines + diagonal glass highlight
    FileBrowser  // file list backdrop -> faint graph-paper grid (subtler, no noise)
};

namespace detail
{
    // Procedural grain/noise is rendered once per (zone, size) into a
    // juce::Image with a fixed-seed juce::Random and reused on every repaint —
    // NOT regenerated per-pixel per-paint, which would be a measurable cost
    // on panels that repaint on a timer (e.g. TransportBar, Sf2ChannelFxPanel).
    inline juce::Image& getGrainImage (PanelZone zone, int w, int h)
    {
        struct CacheEntry { PanelZone zone; int w; int h; juce::Image img; };
        static std::vector<CacheEntry> cache;

        for (auto& e : cache)
            if (e.zone == zone && e.w == w && e.h == h)
                return e.img;

        juce::Image img (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        juce::Graphics gg (img);
        juce::Random rng (0x44594B54u + (uint32_t) zone); // fixed seed -> deterministic, no flicker across repaints

        if (zone == PanelZone::Chassis)
        {
            // Brushed metal: short horizontal streaks of varying alpha, biased
            // so the grain reads as directional brushing rather than static noise.
            for (int y = 0; y < h; ++y)
            {
                const float rowAlpha = rng.nextFloat() * 0.05f;
                gg.setColour (juce::Colours::white.withAlpha (rowAlpha));
                int x = 0;
                while (x < w)
                {
                    const int len = 4 + rng.nextInt (10);
                    if (rng.nextFloat() < 0.5f)
                        gg.drawHorizontalLine (y, (float) x, (float) juce::jmin (w, x + len));
                    x += len + rng.nextInt (6);
                }
            }
        }
        else if (zone == PanelZone::FileBrowser)
        {
            // Faint graph-paper grid at ~3% opacity — deliberately no noise here
            // so it doesn't fight with row scanning in a scrolling list.
            constexpr int step = 8;
            gg.setColour (juce::Colours::white.withAlpha (0.03f));
            for (int x = 0; x < w; x += step) gg.drawVerticalLine (x, 0.0f, (float) h);
            for (int y = 0; y < h; y += step) gg.drawHorizontalLine (y, 0.0f, (float) w);
        }
        else // Instrument
        {
            // Fine, low-alpha grain for the LCD/CRT surface.
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const float a = rng.nextFloat() * 0.035f;
                    if (a > 0.005f)
                    {
                        gg.setColour (juce::Colours::white.withAlpha (a));
                        gg.fillRect (x, y, 1, 1);
                    }
                }
            }
        }

        cache.push_back ({ zone, w, h, img });
        // Bound the cache so repeated resizing across many panels can't grow
        // it without limit; oldest entry is evicted first.
        if (cache.size() > 24)
            cache.erase (cache.begin());
        return cache.back().img;
    }
}

// Layers a base gradient, cached grain, and a radial vignette over `bounds`,
// filled/keyed off `panelColour` — pass whichever colour the panel actually
// fills with (darkBar / header / button), NOT ThemeData::background: 9 of the
// 11 built-in themes set background to pure black, so gradient/vignette math
// on that field is invisible.
//
// Direction (lighten vs darken) is derived from panelColour's own perceived
// brightness rather than hardcoded, so a hypothetical bright custom theme
// (via ThemeData::fromThemeFile()) still reads as "depth" rather than
// "smudge" -- the same style of brightness-driven branch used for contrast
// decisions elsewhere in this file (see stylePrimaryPopupButton's inline
// comment for a case where an alpha-blind brightness check went wrong).
inline void drawTexturedPanel (juce::Graphics& g, juce::Rectangle<float> bounds,
                                juce::Colour panelColour, PanelZone zone,
                                float cornerRadius = 0.0f)
{
    if (bounds.isEmpty())
        return;

    const bool dark = panelColour.getPerceivedBrightness() < 0.5f;

    const auto fillShape = [&] (juce::ColourGradient grad)
    {
        g.setGradientFill (grad);
        if (cornerRadius > 0.0f) g.fillRoundedRectangle (bounds, cornerRadius);
        else                     g.fillRect (bounds);
    };

    // 1) Base gradient — subtle top-lit chassis feel. Light appears to come
    //    from above regardless of whether the panel itself is dark or light.
    const auto topCol    = dark ? panelColour.brighter (0.10f) : panelColour.darker (0.06f);
    const auto bottomCol = dark ? panelColour.darker (0.12f)   : panelColour.brighter (0.05f);
    fillShape (juce::ColourGradient (topCol, 0.0f, bounds.getY(),
                                      bottomCol, 0.0f, bounds.getBottom(), false));

    // 2) Cached grain, clipped to the panel shape.
    {
        auto& grain = detail::getGrainImage (zone, juce::roundToInt (bounds.getWidth()),
                                                     juce::roundToInt (bounds.getHeight()));
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        if (cornerRadius > 0.0f) clip.addRoundedRectangle (bounds, cornerRadius);
        else                     clip.addRectangle (bounds);
        g.reduceClipRegion (clip);
        g.drawImageAt (grain, juce::roundToInt (bounds.getX()), juce::roundToInt (bounds.getY()));
    }

    // 3) Vignette — radial darken toward the edges on dark panels, radial
    //    lighten on bright ones (same brightness branch as above).
    {
        const auto vignetteCol = dark ? juce::Colours::black : juce::Colours::white;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float radius = 0.5f * std::sqrt (bounds.getWidth() * bounds.getWidth()
                                               + bounds.getHeight() * bounds.getHeight());
        juce::ColourGradient vg (juce::Colours::transparentBlack, cx, cy,
                                  vignetteCol.withAlpha (0.16f), cx, cy - radius, true); // radial
        vg.addColour (0.72, juce::Colours::transparentBlack);
        fillShape (vg);
    }

    // 4) Per-zone signature layer.
    if (zone == PanelZone::Instrument)
    {
        // Thin scanlines.
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip;
        if (cornerRadius > 0.0f) clip.addRoundedRectangle (bounds, cornerRadius);
        else                     clip.addRectangle (bounds);
        g.reduceClipRegion (clip);

        g.setColour (juce::Colours::black.withAlpha (0.10f));
        for (int y = juce::roundToInt (bounds.getY()); y < juce::roundToInt (bounds.getBottom()); y += 2)
            g.drawHorizontalLine (y, bounds.getX(), bounds.getRight());

        // Diagonal glass highlight — a soft light band crossing the panel at
        // an angle, like light catching a curved LCD cover. Two overlapping
        // passes give the band body without a hard edge.
        const float bw = bounds.getWidth() * 0.35f;
        juce::Path band;
        band.addQuadrilateral (bounds.getX() - bw,         bounds.getY(),
                                bounds.getX(),              bounds.getY(),
                                bounds.getX() + bw * 0.6f,  bounds.getBottom(),
                                bounds.getX() - bw * 0.4f,  bounds.getBottom());

        g.setColour (juce::Colours::white.withAlpha (dark ? 0.05f : 0.10f));
        g.fillPath (band);

        band.applyTransform (juce::AffineTransform::translation (bounds.getWidth() * 0.42f, 0.0f));
        g.setColour (juce::Colours::white.withAlpha (dark ? 0.03f : 0.06f));
        g.fillPath (band);
    }
}

// Cheap win: a soft outer glow in the accent colour, meant to sit *behind* a
// panel's existing hairline border stroke (draw this first, then the stroke
// on top) so the border reads as lit from within rather than just outlined.
inline void drawPanelGlow (juce::Graphics& g, juce::Rectangle<float> bounds,
                            juce::Colour accentColour, float cornerRadius = 4.0f)
{
    juce::DropShadow glow (accentColour.withAlpha (0.35f), 10, { 0, 0 });
    juce::Path p;
    p.addRoundedRectangle (bounds, cornerRadius);
    glow.drawForPath (g, p);
}

// Shared button styling for popup/overlay dialogs (ConfirmOverlay, RenameOverlay,
// AddZoneOverlay, ArchiveUrlOverlay, SaveSfzOverlay, etc.) so every dialog's
// primary and secondary buttons render with identical colours — including
// text colour — instead of each overlay setting its own copy of these values.
//
// Primary  = the affirmative/confirming action (e.g. "OK", "Trim", "Save", "Add Zone")
// Secondary = the neutral/dismissive action (e.g. "Cancel", "No Thanks", "Clear")
//
// Both render with the SAME fill and text colour per theme -- deliberately
// no longer differentiated by a semi-transparent accent fill on primary.
// That accent fill picked its text colour via bg.getPerceivedBrightness(),
// but getPerceivedBrightness() ignores alpha, so it judged brightness off
// the accent's raw RGB rather than how the 85%-alpha fill actually looked
// once composited over the dialog's dark background. On themes with a
// bright/saturated accent that meant black text got chosen for a button
// that rendered dark enough for black text to disappear into it. Using the
// same opaque T.button fill for both buttons sidesteps the alpha issue
// entirely and keeps every popup's two buttons visually consistent.
inline void stylePrimaryPopupButton (juce::TextButton& b, const ThemeData& T)
{
    b.setColour (juce::TextButton::buttonColourId,  T.button);
    b.setColour (juce::TextButton::textColourOffId, T.foreground);
    b.setColour (juce::TextButton::textColourOnId,  T.foreground);
}

inline void styleSecondaryPopupButton (juce::TextButton& b, const ThemeData& T)
{
    b.setColour (juce::TextButton::buttonColourId,  T.button);
    b.setColour (juce::TextButton::textColourOffId, T.foreground);
    b.setColour (juce::TextButton::textColourOnId,  T.foreground);
}

// Shared popup dialog chrome, used by every overlay (ConfirmOverlay,
// RenameOverlay, AddZoneOverlay, ArchiveUrlOverlay, SaveSfzOverlay,
// MessageOverlay). Flat, square-cornered box, no drop shadow, no glow —
// was unconditionally rendering a 12px rounded corner, a 22px-blur black
// drop shadow, and a 0.7-alpha accent-coloured glow border regardless of
// theme (see the removed comment this replaced: "softer rounded box, a
// real drop shadow for elevation..."). That made every popup in the app
// round-cornered and glowing no matter which theme was active, since none
// of this file's helpers were ever gated on theme.name. Square corners and
// a plain hairline border now match drawAlertBox/drawButtonBackground/
// drawComboBox's flat shape language.
static constexpr float kPopupCornerRadius = 0.0f;

inline void drawPopupBackdrop (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.fillRect (bounds);
}

inline void drawPopupBox (juce::Graphics& g, juce::Rectangle<int> box, const ThemeData& T)
{
    g.setColour (T.header);
    g.fillRoundedRectangle (box.toFloat(), kPopupCornerRadius);
    g.setColour (T.separator);
    g.drawRoundedRectangle (box.toFloat().reduced (0.5f), kPopupCornerRadius, 1.0f);
}

// Computes a new parameter value from a vertical drag gesture.
// startVal    : value at drag start
// deltaY      : upward pixels (positive = increase)
// minVal/maxVal: parameter range
// coarse      : true when Shift is held (5-unit snap; default: 1-unit)
// Sensitivity : full parameter range covered in 200 px of drag
inline float computeDragValue (float startVal, float deltaY,
                                float minVal, float maxVal, bool coarse)
{
    float sensitivity = (maxVal - minVal) / 200.0f;
    float newVal = startVal + deltaY * sensitivity;
    float snap = coarse ? 5.0f : 1.0f;
    newVal = std::round (newVal / snap) * snap;
    return juce::jlimit (minVal, maxVal, newVal);
}

// Computes a zoom multiplier from a vertical drag delta.
// Each pixel of downward drag multiplies zoom by 1.01, giving
// approximately 70 px to double or halve the zoom level.
inline float computeZoomFactor (float deltaY)
{
    return std::pow (1.01f, deltaY);
}

// ── Auto-sizing popup body text ─────────────────────────────────────────────
// Every popup dialog (MessageOverlay, ArchiveMessageOverlay, ConfirmOverlay,
// LoadProgressOverlay, ...) used to size its body-text area off a FIXED
// dialogBox() height and rely on g.drawFittedText()'s font-shrinking
// fallback (or, for ConfirmOverlay, single-line truncation) to cram
// overflow into that fixed space. Fine for a short one-line message, but a
// longer one — e.g. SfzImporter's multi-line import-warnings list — could
// get shrunk down to an illegibly tiny font rather than the box just
// growing to fit it (this is exactly what happened: see the Import
// Completed With Warnings popup this pair of helpers was added to fix).
//
// measureWrappedTextHeight() tells a caller's dialogBox() how tall the
// message actually needs to be, wrapped at a given width, at a FIXED font
// size (no shrinking) — so the box can grow to fit instead. drawWrappedText()
// then renders with that same fixed font, clipped to the area actually
// allotted, so if a truly enormous message still exceeds the caller's own
// max-height clamp, it clips cleanly at a still-legible size rather than
// shrinking to fit or silently truncating mid-word.
inline int measureWrappedTextHeight (const juce::String& text, const juce::Font& font, int width)
{
    if (width <= 0 || text.isEmpty())
        return 0;

    juce::AttributedString as;
    as.setText (text);
    as.setFont (font);

    juce::TextLayout layout;
    layout.createLayout (as, (float) width);
    return (int) std::ceil (layout.getHeight());
}

inline void drawWrappedText (juce::Graphics& g, const juce::String& text, const juce::Font& font,
                              juce::Colour colour, juce::Rectangle<float> area)
{
    juce::AttributedString as;
    as.setText (text);
    as.setFont (font);
    as.setColour (colour);

    juce::TextLayout layout;
    layout.createLayout (as, area.getWidth());

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (area.getSmallestIntegerContainer());
    layout.draw (g, area);
}

} // namespace UIHelpers

namespace UILayout
{

// Waveform vertical scale factor per channel.
// At 0.48, each channel's peak reaches 48% of component height,
// leaving a small visible gap between the two channels at 0 dBFS.
static constexpr float waveformVerticalScale = 0.48f;

} // namespace UILayout
