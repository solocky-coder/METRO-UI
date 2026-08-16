#pragma once
#include "UIHelpers.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

// =============================================================================
//  LoadProgressOverlay — non-interactive status popup shown while a file is
//  downloading (determinate, tracks bytes) or being decoded into the audio
//  engine (indeterminate — sfizz/decoders don't report byte-level progress).
//
//  Reparented to the plugin window's top-level component (see
//  FileBrowserPanel::showLoadProgress), so it floats above BOTH the archive
//  browser and the local filesystem browser — unlike the old approach of
//  drawing progress inside FileBrowserPanel's own preview bar, which only
//  made sense while the archive view happened to be on screen.
// =============================================================================
class LoadProgressOverlay : public juce::Component
{
public:
    explicit LoadProgressOverlay (const juce::String& initialLabel)
        : label (initialLabel)
    {
        // Status-only — clicking through it shouldn't dismiss it; there's
        // nothing for the user to cancel here yet, so don't pretend to be
        // a modal dialog.
        setInterceptsMouseClicks (false, false);
    }

    void setLabel (const juce::String& newLabel)
    {
        if (label == newLabel) return;
        label = newLabel;
        repaint();
    }

    /** fraction < 0 -> indeterminate (fill lit solid); 0..1 -> determinate fill. */
    void setProgress (float fraction)
    {
        if (progress == fraction) return;
        progress = fraction;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();
        UIHelpers::drawPopupBackdrop (g, getLocalBounds());

        const auto box = dialogBox();
        UIHelpers::drawPopupBox (g, box, T);

        const int padX = 18;
        UIHelpers::drawWrappedText (g, label, labelFont(), T.accent,
                    juce::Rectangle<float> ((float) (box.getX() + padX), (float) (box.getY() + 18),
                                             (float) (box.getWidth() - padX * 2), (float) labelHeight()));

        const auto track = juce::Rectangle<int> (box.getX() + padX, box.getBottom() - 30,
                                                   box.getWidth() - padX * 2, 6);
        g.setColour (T.darkBar.darker (0.4f));
        g.fillRoundedRectangle (track.toFloat(), 3.0f);

        g.setColour (T.accent);
        if (progress >= 0.0f)
        {
            const int fillW = juce::jmax (6, (int) (track.getWidth() * juce::jlimit (0.0f, 1.0f, progress)));
            g.fillRoundedRectangle (track.withWidth (fillW).toFloat(), 3.0f);
        }
        else
        {
            // Unknown total / non-byte-based phase — light the whole track
            // rather than implying a fake percentage.
            g.fillRoundedRectangle (track.toFloat(), 3.0f);
        }
    }

    void resized() override {}

private:
    juce::String label;
    float        progress = 0.0f;

    static juce::Font labelFont() { return DysektLookAndFeel::makeFont (13.0f, true); }

    int labelHeight() const
    {
        const int padX = 18;
        const int w = juce::jmin (360, getWidth() - 40);
        const int textWidth = juce::jmax (10, w - padX * 2);
        // Same 2-line cap as the old drawFittedText call — a status label
        // this long was always going to be unusual, so still bound it
        // rather than let a pathological string blow the popup out; unlike
        // before, though, it wraps/clips at a fixed legible size instead of
        // shrinking to fit.
        return juce::jmin (UIHelpers::measureWrappedTextHeight (label, labelFont(), textWidth),
                            (int) (labelFont().getHeight() * 2 + 4));
    }

    juce::Rectangle<int> dialogBox() const
    {
        const int w = juce::jmin (360, getWidth() - 40);

        // 18 = top offset of the label (matches paint()); 30 = space below
        // it reserved for the progress track + bottom margin (matches the
        // track's own "box.getBottom() - 30" placement below).
        const int contentH = 18 + labelHeight() + 30;

        const int maxH = juce::jmax (96, getHeight() - 40);
        const int h    = juce::jlimit (96, maxH, contentH);

        return { (getWidth() - w) / 2, (getHeight() - h) / 2, w, h };
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoadProgressOverlay)
};
