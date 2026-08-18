#pragma once
#include "UIHelpers.h"
// =============================================================================
//  AddZonePanel.h  —  Floating desktop window: configure a new SFZ zone
// =============================================================================
//  Replaces AddZoneOverlay's centered-card-painted-over-the-editor approach
//  with a genuine floating window, built the same way as ThemeEditorPanel:
//  addToDesktop + its own draggable title bar + resizable border/corner +
//  remembered position/size across sessions.
//
//  Shown after the user picks a sample file.  Lets them set loKey / hiKey /
//  rootKey before the <region> is written, preventing accidental overlaps.
//
//  Default: single key (loKey == hiKey == prevHiKey + 1), root == loKey.
//  The user can expand the range before confirming.
//
//  onResult (loKey, hiKey, rootKey, confirmed)
//      confirmed == false  →  user cancelled; do not write anything.
//  onDismiss()
//      Fired when the window is closed via [X] or Esc — mirrors
//      ThemeEditorPanel::onDismiss. The host is responsible for dropping its
//      unique_ptr in response (see MultisamplerEditor's own AddZoneOverlay
//      handling for the pattern).
//
//  v1 scope: title bar + resizing + the three existing spinner rows, laid
//  out with more room than the 520x300 card had. No waveform preview or
//  mini keyboard yet — see AddZonePanel-implementation.md section 6 for the
//  v2 follow-up plan.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "DysektLookAndFeel.h"

class AddZonePanel : public juce::Component
{
public:
    /** lo, hi, root are MIDI note numbers (0-127).  confirmed=false = cancel. */
    std::function<void (int lo, int hi, int root, bool confirmed)> onResult;

    /** Window closed via [X] or Esc. */
    std::function<void()> onDismiss;

    /** Which key field, if any, is currently armed for MIDI learn. */
    enum class LearnTarget
    {
        none,
        lowKey,
        highKey,
        rootKey
    };

    /** @param sampleFile  the sample this zone will be built from; its bare
     *                     filename is shown in the title bar subtitle
     *  @param defaultLo   suggested loKey (= prevHiKey + 1, or 0 if first zone)
     */
    AddZonePanel (const juce::File& sampleFile_, int defaultLo)
        : sampleFile (sampleFile_),
          loKey  (juce::jlimit (0, 127, defaultLo)),
          hiKey  (juce::jlimit (0, 127, defaultLo)),   // single-key default
          rootKey(juce::jlimit (0, 127, defaultLo))
    {
        const auto& T = getTheme();

        setOpaque (true);
        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);

        // Prevent the PointingHandCursor set on KeysPanel from bleeding
        // through when JUCE walks up the component hierarchy to resolve the
        // cursor (same guard AddZoneOverlay had).
        setMouseCursor (juce::MouseCursor::NormalCursor);

        // ── Title bar ────────────────────────────────────────────────────
        titleLabel.setText ("ADD ZONE", juce::dontSendNotification);
        titleLabel.setFont (DysektLookAndFeel::makeFont (17.0f, true));
        titleLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (titleLabel);

        subtitleLabel.setText (sampleFile.getFileName(), juce::dontSendNotification);
        subtitleLabel.setFont (DysektLookAndFeel::makeFont (11.5f));
        subtitleLabel.setColour (juce::Label::textColourId, T.foreground.withAlpha (0.6f));
        subtitleLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (subtitleLabel);

        closeBtn.setButtonText ("X");
        closeBtn.onClick = [this] { fire (false); };
        addAndMakeVisible (closeBtn);

        // ── Spinner rows ─────────────────────────────────────────────────
        auto styleSpinner = [&] (juce::TextButton& dn, juce::TextButton& up)
        {
            for (auto* b : { &dn, &up })
            {
                b->setColour (juce::TextButton::buttonColourId,  T.darkBar);
                b->setColour (juce::TextButton::textColourOffId, T.foreground);
                b->setMouseCursor (juce::MouseCursor::NormalCursor);
                addAndMakeVisible (*b);
            }
        };

        styleSpinner (loDown, loUp);
        styleSpinner (hiDown, hiUp);
        styleSpinner (rtDown, rtUp);

        loDown.setButtonText ("<");  loUp.setButtonText (">");
        hiDown.setButtonText ("<");  hiUp.setButtonText (">");
        rtDown.setButtonText ("<");  rtUp.setButtonText (">");

        // Manual arrows remain available for correction; using one on a row
        // cancels that row's active learn state (spec section 6).
        loDown.onClick = [this] { cancelLearnFor (LearnTarget::lowKey);  adjust (loKey, -1, true);  };
        loUp  .onClick = [this] { cancelLearnFor (LearnTarget::lowKey);  adjust (loKey, +1, true);  };
        hiDown.onClick = [this] { cancelLearnFor (LearnTarget::highKey); adjust (hiKey, -1, false); };
        hiUp  .onClick = [this] { cancelLearnFor (LearnTarget::highKey); adjust (hiKey, +1, false); };
        rtDown.onClick = [this] { cancelLearnFor (LearnTarget::rootKey); rootKey = juce::jlimit (0, 127, rootKey - 1); repaint(); };
        rtUp  .onClick = [this] { cancelLearnFor (LearnTarget::rootKey); rootKey = juce::jlimit (0, 127, rootKey + 1); repaint(); };

        // ── MIDI-learn buttons ───────────────────────────────────────────
        // One per row, placed between the readout and the up-arrow (see
        // clusterRows()). Selecting a button arms that row's learn target;
        // selecting a different one cancels the previous target; clicking
        // the already-armed button again cancels learning.
        for (auto* b : { &loLearnButton, &hiLearnButton, &rootLearnButton })
        {
            b->setColour (juce::TextButton::buttonColourId,  T.darkBar);
            b->setColour (juce::TextButton::textColourOffId, T.foreground);
            b->setMouseCursor (juce::MouseCursor::NormalCursor);
            addAndMakeVisible (*b);
        }

        loLearnButton  .onClick = [this] { toggleLearnTarget (LearnTarget::lowKey);  };
        hiLearnButton  .onClick = [this] { toggleLearnTarget (LearnTarget::highKey); };
        rootLearnButton.onClick = [this] { toggleLearnTarget (LearnTarget::rootKey); };

        updateLearnButtonCaptions();

        // ── Confirm / Cancel ─────────────────────────────────────────────
        UIHelpers::stylePrimaryPopupButton   (confirmBtn, T);
        UIHelpers::styleSecondaryPopupButton (cancelBtn,  T);
        addAndMakeVisible (confirmBtn);
        addAndMakeVisible (cancelBtn);

        confirmBtn.setButtonText ("ADD ZONE");
        cancelBtn .setButtonText ("CANCEL");

        confirmBtn.onClick = [this] { fire (true);  };
        cancelBtn .onClick = [this] { fire (false); };

        confirmBtn.setMouseCursor (juce::MouseCursor::NormalCursor);
        cancelBtn .setMouseCursor (juce::MouseCursor::NormalCursor);

        // ── Resizing ─────────────────────────────────────────────────────
        resizeConstrainer.setSizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);

        resizableBorder = std::make_unique<juce::ResizableBorderComponent> (this, &resizeConstrainer);
        addAndMakeVisible (*resizableBorder);

        resizableCorner = std::make_unique<juce::ResizableCornerComponent> (this, &resizeConstrainer);
        addAndMakeVisible (*resizableCorner);

        setSize (kDefaultWidth, kDefaultHeight);
    }

    ~AddZonePanel() override
    {
        if (isOnDesktop())
            savePosition();
    }

    void show()
    {
        if (! isOnDesktop())
        {
            addToDesktop (juce::ComponentPeer::windowHasDropShadow);
            restorePosition();
        }

        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
    }

    void hide()
    {
        if (! isOnDesktop())
            return;

        savePosition();
        removeFromDesktop();
    }

    bool isFloating() const noexcept { return isOnDesktop(); }

    /** Message-thread-only. Forwards one newly-observed MIDI note-on from
     *  PluginEditor's UI-timer poll of the processor's MIDI-learn snapshot
     *  (see PluginEditor::timerCallback). No-op if no target is armed. */
    void acceptMidiLearnNote (int midiNote)
    {
        midiNote = juce::jlimit (0, 127, midiNote);

        switch (learnTarget)
        {
            case LearnTarget::lowKey:
                loKey = midiNote;
                if (loKey > hiKey)
                    hiKey = loKey;
                break;

            case LearnTarget::highKey:
                hiKey = midiNote;
                if (hiKey < loKey)
                    loKey = hiKey;
                break;

            case LearnTarget::rootKey:
                // Intentionally NOT clamped into [loKey, hiKey] — SFZ permits
                // a pitch center outside the played range, and advanced users
                // may intentionally use it. This differs from adjust(), which
                // does clamp root when lo/hi move via the manual arrows.
                rootKey = midiNote;
                break;

            case LearnTarget::none:
                return;
        }

        learnTarget = LearnTarget::none;
        updateLearnButtonCaptions();
        repaint();
    }

    // ── Layout ────────────────────────────────────────────────────────────
    void resized() override
    {
        if (resizableBorder != nullptr)
        {
            resizableBorder->setBounds (getLocalBounds());
            resizableBorder->toFront (false);
        }
        if (resizableCorner != nullptr)
        {
            constexpr int kCornerSize = 16;
            resizableCorner->setBounds (getWidth() - kCornerSize, getHeight() - kCornerSize,
                                         kCornerSize, kCornerSize);
            resizableCorner->toFront (false);
        }

        dialogBounds = getLocalBounds();
        titleBarBounds = dialogBounds.withHeight (kTitleBarH);

        auto db = dialogBounds;
        auto titleBar = db.removeFromTop (kTitleBarH);
        closeBtn.setBounds (titleBar.removeFromRight (36).reduced (6));
        titleBar.removeFromLeft (16);
        titleLabel   .setBounds (titleBar.removeFromTop (titleBar.getHeight() / 2));
        subtitleLabel.setBounds (titleBar);

        auto btnRow = db.removeFromBottom (kButtonRowH).reduced (18, 10);
        const int btnH  = 34;
        const int btnW  = 130;
        const int gap   = 12;
        const int totalW = btnW * 2 + gap;
        const int btnX  = btnRow.getCentreX() - totalW / 2;
        confirmBtn.setBounds (btnX,             btnRow.getY(), btnW, btnH);
        cancelBtn .setBounds (btnX + btnW + gap, btnRow.getY(), btnW, btnH);

        hintBounds = db.removeFromBottom (kHintH);

        // Spinner rows flex to fill whatever's left — stretched horizontally,
        // fixed row height, centred as a block within the available width.
        rowsArea = db.reduced (24, 12);

        const auto rows = clusterRows();
        juce::TextButton* dn[]    = { &loDown, &hiDown, &rtDown };
        juce::TextButton* up[]    = { &loUp,   &hiUp,   &rtUp   };
        juce::TextButton* learn[] = { &loLearnButton, &hiLearnButton, &rootLearnButton };

        for (int i = 0; i < 3; ++i)
        {
            const auto& c = rows[i];
            dn[i]->setBounds (c.downX, c.y + 3, kArrowW, kRowH - 6);
            learn[i]->setBounds (c.learnX, c.y + 5, kLearnW, kRowH - 10);
            up[i]->setBounds (c.upX,   c.y + 3, kArrowW, kRowH - 6);
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();

        g.fillAll (T.darkBar.darker (0.5f));
        g.setColour (T.header);
        g.fillRect (dialogBounds);

        auto titleBar = dialogBounds.withHeight (kTitleBarH);
        g.setColour (T.darkBar.darker (0.4f));
        g.fillRect (titleBar);

        g.setColour (T.accent.withAlpha (0.55f));
        g.drawRect (dialogBounds, 1);
        g.setColour (T.separator.withAlpha (0.5f));
        g.drawHorizontalLine (dialogBounds.getY() + kTitleBarH,
                              (float) dialogBounds.getX(), (float) dialogBounds.getRight());

        // Row labels + note readouts
        const auto rows = clusterRows();
        const char* labels[] = { "loKey", "hiKey", "root" };
        const int   vals[]   = { loKey,   hiKey,   rootKey };

        for (int i = 0; i < 3; ++i)
        {
            const auto& c = rows[i];

            g.setFont (DysektLookAndFeel::makeFont (13.5f, true));
            g.setColour (T.foreground.withAlpha (0.85f));
            g.drawText (labels[i],
                        c.labelX, c.y,
                        kLabelW, kRowH,
                        juce::Justification::centredLeft, false);

            g.setFont (DysektLookAndFeel::makeFont (19.0f, true));
            g.setColour (T.foreground);
            g.drawText (noteName (vals[i]),
                        c.readoutX, c.y,
                        kReadoutW, kRowH,
                        juce::Justification::centred, false);

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
        g.drawText (getHintText(), hintBounds, juce::Justification::centred, false);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            // Escape first cancels an armed learn target; only closes/cancels
            // the whole window if nothing was armed (spec section 3).
            if (learnTarget != LearnTarget::none)
            {
                learnTarget = LearnTarget::none;
                updateLearnButtonCaptions();
                repaint();
                return true;
            }

            fire (false);
            return true;
        }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto closeButtonBounds = titleBarBounds.removeFromRight (36).reduced (6);
        draggingTitleBar = titleBarBounds.contains (e.getPosition())
                         && ! closeButtonBounds.contains (e.getPosition());

        if (draggingTitleBar)
            dragger.startDraggingComponent (this, e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingTitleBar)
            dragger.dragComponent (this, e, nullptr);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingTitleBar = false;
    }

private:
    // Widened vs. the pre-MIDI-learn v1 (560x480 / min 480x380 / max 900x700)
    // to fit the LEARN button in each row without the MIDI controls
    // overlapping at the minimum resizable size (see clusterRows() below).
    static constexpr int kDefaultWidth  = 660;
    static constexpr int kDefaultHeight = 480;
    static constexpr int kMinWidth      = 560;
    static constexpr int kMinHeight     = 380;
    static constexpr int kMaxWidth      = 980;
    static constexpr int kMaxHeight     = 700;

    static constexpr int kTitleBarH  = 40;
    static constexpr int kButtonRowH = 54;
    static constexpr int kHintH      = 30;

    static constexpr int kArrowW   = 30;
    static constexpr int kReadoutW = 64;
    static constexpr int kLearnW   = 86;   // fits "PLAY NOTE…"
    static constexpr int kRowH     = 46;
    static constexpr int kLabelW   = 60;
    static constexpr int kSubW     = 40;

    juce::File sampleFile;
    int loKey, hiKey, rootKey;

    LearnTarget learnTarget = LearnTarget::none;

    juce::Label      titleLabel, subtitleLabel;
    juce::TextButton closeBtn;

    juce::TextButton loDown, loUp;
    juce::TextButton hiDown, hiUp;
    juce::TextButton rtDown, rtUp;
    juce::TextButton loLearnButton, hiLearnButton, rootLearnButton;
    juce::TextButton confirmBtn, cancelBtn;

    juce::ComponentBoundsConstrainer  resizeConstrainer;
    std::unique_ptr<juce::ResizableBorderComponent> resizableBorder;
    std::unique_ptr<juce::ResizableCornerComponent> resizableCorner;

    juce::ComponentDragger dragger;
    bool draggingTitleBar = false;

    juce::Rectangle<int> dialogBounds, titleBarBounds, rowsArea, hintBounds;

    // ── Helpers ───────────────────────────────────────────────────────────

    // Matches the app-wide convention (C3 == MIDI 60, octave = note/12 - 2)
    // used elsewhere in the codebase (SliceLcdDisplay.cpp, SliceControlBar.cpp,
    // KeysPanel.cpp, SfzLcdDisplay.cpp, SfzPlayerDropdownPanel.cpp).
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
        // Close, Cancel, and Add Zone must all clear any armed learn target.
        learnTarget = LearnTarget::none;

        if (onResult)
            onResult (loKey, hiKey, rootKey, confirmed);
    }

    /** Cancels the given row's learn target if it's the one currently armed;
     *  no-op otherwise. Used when a manual arrow is used on that row. */
    void cancelLearnFor (LearnTarget target)
    {
        if (learnTarget == target)
        {
            learnTarget = LearnTarget::none;
            updateLearnButtonCaptions();
        }
    }

    /** Selecting a learn button arms its target; selecting the currently
     *  armed one again cancels learning; selecting a different one moves
     *  the armed target (implicitly cancelling the previous one). */
    void toggleLearnTarget (LearnTarget target)
    {
        learnTarget = (learnTarget == target) ? LearnTarget::none : target;
        updateLearnButtonCaptions();
        repaint();
    }

    /** Restyles the three learn buttons: the armed one gets the panel's
     *  accent colour and a "PLAY NOTE…" caption; the other two show the
     *  normal "LEARN" caption in the default button colour. */
    void updateLearnButtonCaptions()
    {
        const auto& T = getTheme();
        struct Row { juce::TextButton* button; LearnTarget target; };
        const Row rows[] = { { &loLearnButton,   LearnTarget::lowKey  },
                              { &hiLearnButton,   LearnTarget::highKey },
                              { &rootLearnButton, LearnTarget::rootKey } };

        for (const auto& r : rows)
        {
            const bool armed = (learnTarget == r.target);
            r.button->setButtonText (armed ? "PLAY NOTE\xe2\x80\xa6" : "LEARN");
            r.button->setColour (juce::TextButton::buttonColourId,
                                  armed ? T.accent : T.darkBar);
            r.button->setColour (juce::TextButton::textColourOffId,
                                  armed ? T.background : T.foreground);
        }
    }

    /** Hint text under the button row: the normal instructions, or a
     *  learn-specific prompt while a target is armed (spec section 3). */
    juce::String getHintText() const
    {
        if (learnTarget != LearnTarget::none)
            return "Select LEARN, then play one MIDI note.";
        return "Default is one key. Expand the range before confirming.";
    }

    /** One row's laid-out x-positions, centred as a block within rowsArea. */
    struct ClusterRow
    {
        int y, labelX, downX, readoutX, learnX, upX, subX;
    };

    /** Three evenly-spaced rows for lo / hi / root, each centred horizontally
        as a single label+spinner+readout+LEARN+spinner+subscript cluster,
        vertically centred as a block within rowsArea so extra window height
        just adds breathing room rather than stretching the rows themselves.
        LEARN sits between the readout (the displayed value) and the up
        arrow, so the `<` `>` pair still brackets the readout on the outside
        while LEARN reads as the readout's companion control. */
    std::array<ClusterRow, 3> clusterRows() const
    {
        constexpr int gapLblArrow = 10;
        constexpr int gapArrowRdt = 10;
        constexpr int gapRdtLearn = 10;
        constexpr int gapLearnUp  = 10;
        constexpr int gapArrowSub = 14;

        constexpr int clusterW = kLabelW + gapLblArrow + kArrowW + gapArrowRdt
                                + kReadoutW + gapRdtLearn + kLearnW + gapLearnUp
                                + kArrowW + gapArrowSub + kSubW;

        constexpr int gapRows = 6;
        constexpr int blockH  = kRowH * 3 + gapRows * 2;

        const int startY = rowsArea.getY() + juce::jmax (0, (rowsArea.getHeight() - blockH) / 2);
        const int rowX   = rowsArea.getCentreX() - clusterW / 2;

        std::array<ClusterRow, 3> r;
        for (int i = 0; i < 3; ++i)
        {
            const int y        = startY + i * (kRowH + gapRows);
            const int labelX   = rowX;
            const int downX    = labelX + kLabelW + gapLblArrow;
            const int readoutX = downX  + kArrowW  + gapArrowRdt;
            const int learnX   = readoutX + kReadoutW + gapRdtLearn;
            const int upX      = learnX + kLearnW + gapLearnUp;
            const int subX     = upX + kArrowW + gapArrowSub;

            r[i] = { y, labelX, downX, readoutX, learnX, upX, subX };
        }
        return r;
    }

    // ── Position/size persistence (mirrors ThemeEditorPanel) ──────────────
    static juce::File getPositionFile()
    {
        // Same settings dir PluginEditor.cpp's file-local getSettingsDir()
        // resolves to (userApplicationDataDirectory/DYSEKT-SF) — sits
        // alongside theme_editor_position.xml there.
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("DYSEKT-SF")
            .getChildFile ("add_zone_panel_position.xml");
    }

    void restorePosition()
    {
        int x = 160, y = 160;
        int w = kDefaultWidth, h = kDefaultHeight;
        const auto file = getPositionFile();

        if (file.existsAsFile())
            if (auto xml = juce::XmlDocument::parse (file))
            {
                x = xml->getIntAttribute ("x", x);
                y = xml->getIntAttribute ("y", y);
                w = xml->getIntAttribute ("w", w);
                h = xml->getIntAttribute ("h", h);
            }

        w = juce::jlimit (kMinWidth, kMaxWidth, w);
        h = juce::jlimit (kMinHeight, kMaxHeight, h);
        setSize (w, h);

        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* display = displays.getDisplayForPoint (juce::Point<int> (x, y));
        const auto area = display != nullptr ? display->userArea
                                             : juce::Rectangle<int> (0, 0, 1920, 1080);
        x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - getWidth()), x);
        y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - getHeight()), y);
        setTopLeftPosition (x, y);
    }

    void savePosition() const
    {
        const auto file = getPositionFile();
        file.getParentDirectory().createDirectory();

        juce::XmlElement xml ("ADD_ZONE_PANEL_POSITION");
        xml.setAttribute ("x", getX());
        xml.setAttribute ("y", getY());
        xml.setAttribute ("w", getWidth());
        xml.setAttribute ("h", getHeight());
        xml.writeTo (file);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddZonePanel)
};
