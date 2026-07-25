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
        g.setFont (DysektLookAndFeel::makeFont (13.0f, true));
        g.setColour (T.accent);
        g.drawFittedText (label,
                          box.getX() + padX, box.getY() + 18,
                          box.getWidth() - padX * 2, 42,
                          juce::Justification::centredLeft, 2);

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

    juce::Rectangle<int> dialogBox() const
    {
        const int w = juce::jmin (360, getWidth() - 40);
        const int h = 96;
        return { (getWidth() - w) / 2, (getHeight() - h) / 2, w, h };
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoadProgressOverlay)
};
