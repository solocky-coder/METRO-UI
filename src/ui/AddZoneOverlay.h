#pragma once
#include "UIHelpers.h"
// =============================================================================
//  AddZoneOverlay.h  —  In-plugin dialog: configure a new SFZ zone
// =============================================================================
//  Shown after the user picks a sample file.  Lets them set loKey / hiKey /
//  rootKey before the <region> is written, preventing accidental overlaps.
//
//  Default: single key (loKey == hiKey == prevHiKey + 1), root == loKey.
//  The user can expand the range before confirming.
//
//  onResult (loKey, hiKey, rootKey, confirmed)
//      confirmed == false  →  user cancelled; do not write anything.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

class AddZoneOverlay : public juce::Component
{
public:
    /** lo, hi, root are MIDI note numbers (0-127).  confirmed=false = cancel. */
    std::function<void (int lo, int hi, int root, bool confirmed)> onResult;

    /** @param sampleName  bare filename shown in the title bar
     *  @param defaultLo   suggested loKey (= prevHiKey + 1, or 0 if first zone)
     */
    AddZoneOverlay (const juce::String& sampleName, int defaultLo)
        : loKey  (juce::jlimit (0, 127, defaultLo)),
          hiKey  (juce::jlimit (0, 127, defaultLo)),   // single-key default
          rootKey(juce::jlimit (0, 127, defaultLo)),
          title  ("ADD ZONE  —  " + sampleName.toUpperCase())
    {
        const auto& T = getTheme();

        auto styleBtn = [&] (juce::TextButton& b, bool accent)
        {
            if (accent)
                UIHelpers::stylePrimaryPopupButton (b, T);
            else
                UIHelpers::styleSecondaryPopupButton (b, T);
            addAndMakeVisible (b);
        };

        auto styleSpinner = [&] (juce::TextButton& dn, juce::TextButton& up)
        {
            for (auto* b : { &dn, &up })
            {
                b->setColour (juce::TextButton::buttonColourId,  T.darkBar);
                b->setColour (juce::TextButton::textColourOffId, T.foreground);
                addAndMakeVisible (*b);
            }
        };

        styleSpinner (loDown, loUp);
        styleSpinner (hiDown, hiUp);
        styleSpinner (rtDown, rtUp);

        loDown.setButtonText ("<");  loUp.setButtonText (">");
        hiDown.setButtonText ("<");  hiUp.setButtonText (">");
        rtDown.setButtonText ("<");  rtUp.setButtonText (">");

        loDown.onClick = [this] { adjust (loKey, -1, true);  };
        loUp  .onClick = [this] { adjust (loKey, +1, true);  };
        hiDown.onClick = [this] { adjust (hiKey, -1, false); };
        hiUp  .onClick = [this] { adjust (hiKey, +1, false); };
        rtDown.onClick = [this] { rootKey = juce::jlimit (0, 127, rootKey - 1); repaint(); };
        rtUp  .onClick = [this] { rootKey = juce::jlimit (0, 127, rootKey + 1); repaint(); };

        styleBtn (confirmBtn, true);
        styleBtn (cancelBtn,  false);

        confirmBtn.setButtonText ("ADD ZONE");
        cancelBtn .setButtonText ("CANCEL");

        confirmBtn.onClick = [this] { fire (true);  };
        cancelBtn .onClick = [this] { fire (false); };

        setInterceptsMouseClicks (true, true);

        // Prevent the PointingHandCursor set on KeysPanel from bleeding through
        // when JUCE walks up the component hierarchy to resolve the cursor.
        setMouseCursor (juce::MouseCursor::NormalCursor);
        for (auto* b : { &loDown, &loUp, &hiDown, &hiUp, &rtDown, &rtUp,
                         &confirmBtn, &cancelBtn })
            b->setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();

        UIHelpers::drawPopupBackdrop (g, getLocalBounds());

        const auto box = dialogBox();

        // Card
        UIHelpers::drawPopupBox (g, box, T);

        const int padX = 18;

        // Title
        g.setFont (DysektLookAndFeel::makeFont (16.0f, true));
        g.setColour (T.accent);
        g.drawText (title,
                    box.getX() + padX, box.getY() + 16,
                    box.getWidth() - padX * 2, 24,
                    juce::Justification::centredLeft, true);


        // Row labels + note readouts
        const auto rows = clusterRows (box);

        const char* labels[] = { "loKey", "hiKey", "root" };
        const int   vals[]   = { loKey,   hiKey,   rootKey };

        for (int i = 0; i < 3; ++i)
        {
            const auto& c = rows[i];

            // Label
            g.setFont (DysektLookAndFeel::makeFont (13.5f, true));
            g.setColour (T.foreground.withAlpha (0.85f));
            g.drawText (labels[i],
                        c.labelX, c.y,
                        kLabelW, kRowH,
                        juce::Justification::centredLeft, false);

            // Note name readout (centred between the two spinner buttons)
            g.setFont (DysektLookAndFeel::makeFont (19.0f, true));
            g.setColour (T.foreground);
            g.drawText (noteName (vals[i]),
                        c.readoutX, c.y,
                        kReadoutW, kRowH,
                        juce::Justification::centred, false);

            // MIDI number subscript
            g.setFont (DysektLookAndFeel::makeFont (11.0f));
            g.setColour (T.foreground.withAlpha (0.55f));
            g.drawText ("(" + juce::String (vals[i]) + ")",
                        c.subX, c.y,
                        kSubW, kRowH,
                        juce::Justification::centredLeft, false);
        }

        // Hint line
        g.setFont (DysektLookAndFeel::makeFont (11.5f));
        g.setColour (T.foreground.withAlpha (0.55f));
        g.drawText ("Default is one key. Expand the range before confirming.",
                    box.getX() + padX,
                    rowsBottom (box) + 10,
                    box.getWidth() - padX * 2,
                    18,
                    juce::Justification::centred, false);
    }

    void resized() override
    {
        const auto box  = dialogBox();
        const auto rows = clusterRows (box);

        juce::TextButton* dn[] = { &loDown, &hiDown, &rtDown };
        juce::TextButton* up[] = { &loUp,   &hiUp,   &rtUp   };

        for (int i = 0; i < 3; ++i)
        {
            const auto& c = rows[i];
            dn[i]->setBounds (c.downX, c.y + 3, kArrowW, kRowH - 6);
            up[i]->setBounds (c.upX,   c.y + 3, kArrowW, kRowH - 6);
        }

        // Confirm / Cancel buttons
        const int btnH  = 34;
        const int btnW  = 130;
        const int gap   = 12;
        const int btnY  = box.getBottom() - btnH - 14;
        const int totalW = btnW * 2 + gap;
        const int btnX  = box.getCentreX() - totalW / 2;

        confirmBtn.setBounds (btnX,           btnY, btnW, btnH);
        cancelBtn .setBounds (btnX + btnW + gap, btnY, btnW, btnH);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! dialogBox().contains (e.getPosition()))
            fire (false);
    }

private:
    static constexpr int kArrowW   = 30;
    static constexpr int kReadoutW = 64;
    static constexpr int kRowH     = 42;
    static constexpr int kLabelW   = 60;
    static constexpr int kSubW     = 40;

    int loKey, hiKey, rootKey;
    juce::String title;

    juce::TextButton loDown, loUp;
    juce::TextButton hiDown, hiUp;
    juce::TextButton rtDown, rtUp;
    juce::TextButton confirmBtn, cancelBtn;

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Matches the app-wide convention (C3 == MIDI 60, octave = note/12 - 2)
    // used by SliceLcdDisplay.cpp, SliceControlBar.cpp, KeysPanel.cpp,
    // SfzLcdDisplay.cpp, and SfzPlayerDropdownPanel.cpp's noteStr()/
    // parseSfzKey(). Was note/12-1 (one octave off from the rest of the
    // app) -- this is a pure display readout in this overlay, so the fix
    // only changes the label shown while adjusting lo/hi/root key, not any
    // stored MIDI note value.
    static juce::String noteName (int n)
    {
        static const char* names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String (names[n % 12]) + juce::String (n / 12 - 2);
    }

    /** Adjust a key value, clamping and keeping lo <= hi. */
    void adjust (int& val, int delta, bool isLo)
    {
        val = juce::jlimit (0, 127, val + delta);
        if (isLo && loKey > hiKey) hiKey  = loKey;
        if (!isLo && hiKey < loKey) loKey = hiKey;
        // Keep root inside [lo, hi]
        rootKey = juce::jlimit (loKey, hiKey, rootKey);
        repaint();
    }

    void fire (bool confirmed)
    {
        if (onResult)
            onResult (loKey, hiKey, rootKey, confirmed);
    }

    juce::Rectangle<int> dialogBox() const
    {
        const int w = juce::jmin (520, getWidth() - 40);
        const int h = 300;
        return juce::Rectangle<int> (
            (getWidth()  - w) / 2,
            (getHeight() - h) / 2,
            w, h);
    }

    /** One row's laid-out x-positions, centred as a block within the box. */
    struct ClusterRow
    {
        int y, labelX, downX, readoutX, upX, subX;
    };

    /** Three evenly-spaced rows for lo / hi / root, each centred horizontally
        as a single label+spinner+readout+subscript cluster (not left-aligned
        to the box edge). */
    std::array<ClusterRow, 3> clusterRows (const juce::Rectangle<int>& box) const
    {
        constexpr int gapLblArrow = 10;
        constexpr int gapArrowRdt = 10;
        constexpr int gapRdtArrow = 10;
        constexpr int gapArrowSub = 14;

        constexpr int clusterW = kLabelW + gapLblArrow + kArrowW + gapArrowRdt
                                + kReadoutW + gapRdtArrow + kArrowW + gapArrowSub + kSubW;

        const int startY = box.getY() + 46;
        const int rowX   = box.getCentreX() - clusterW / 2;

        std::array<ClusterRow, 3> r;
        for (int i = 0; i < 3; ++i)
        {
            const int y       = startY + i * (kRowH + 4);
            const int labelX  = rowX;
            const int downX   = labelX + kLabelW + gapLblArrow;
            const int readoutX= downX  + kArrowW  + gapArrowRdt;
            const int upX     = readoutX + kReadoutW + gapRdtArrow;
            const int subX    = upX + kArrowW + gapArrowSub;

            r[i] = { y, labelX, downX, readoutX, upX, subX };
        }
        return r;
    }

    /** Bottom y of the last cluster row, for placing the hint line beneath it. */
    int rowsBottom (const juce::Rectangle<int>& box) const
    {
        return clusterRows (box)[2].y + kRowH;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddZoneOverlay)
};
