#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

// ============================================================
//  ThemeEditorPanel
//  Full-screen modal overlay.  Lets the user create, modify
//  and save .dsk theme files without leaving the plugin.
//
//  Callbacks the host (PluginEditor) must wire up:
//    onDismiss        — called when the panel closes
//    onThemeChanged   — called live as colours are edited
//    onThemeSaved     — called after a successful save; passes
//                       the new theme name so the editor can
//                       reload its theme list
// ============================================================

class ThemeEditorPanel : public juce::Component,
                         public juce::ScrollBar::Listener
{
public:
    std::function<void()>                   onDismiss;
    std::function<void(const ThemeData&)>   onThemeChanged;
    std::function<void(const juce::String&)> onThemeSaved;

    // ctor takes the themes directory so it can enumerate existing themes
    // and write new .dsk files there.
    explicit ThemeEditorPanel (const juce::File& themesDirectory)
        : themesDir (themesDirectory)
    {
        setInterceptsMouseClicks (true, true);

        // ── Title bar ────────────────────────────────────────────────────
        titleLabel.setText ("THEME EDITOR", juce::dontSendNotification);
        titleLabel.setFont (DysektLookAndFeel::makeFont (14.0f, true));
        addAndMakeVisible (titleLabel);

        closeBtn.setButtonText ("X");
        closeBtn.onClick = [this] { if (onDismiss) onDismiss(); };
        addAndMakeVisible (closeBtn);

        // ── Base theme selector ───────────────────────────────────────────
        baseLabel.setText ("BASE THEME:", juce::dontSendNotification);
        baseLabel.setFont (DysektLookAndFeel::makeFont (10.0f));
        addAndMakeVisible (baseLabel);

        baseCombo.addItem ("(current)",  1);
        populateBaseCombo();
        baseCombo.setSelectedId (1, juce::dontSendNotification);
        baseCombo.onChange = [this] { loadBaseTheme(); };
        addAndMakeVisible (baseCombo);

        // ── Theme name input ──────────────────────────────────────────────
        nameLabel.setText ("NAME:", juce::dontSendNotification);
        nameLabel.setFont (DysektLookAndFeel::makeFont (10.0f));
        addAndMakeVisible (nameLabel);

        nameEditor.setMultiLine (false);
        nameEditor.setReturnKeyStartsNewLine (false);
        nameEditor.setText ("my-theme");
        addAndMakeVisible (nameEditor);

        // ── Colour rows ───────────────────────────────────────────────────
        // Structural colours
        addRow ("BACKGROUND",   "background");
        addRow ("WAVEFORM BG",  "waveformBg");
        addRow ("DARK BAR",     "darkBar");
        addRow ("HEADER",       "header");
        addRow ("FOREGROUND",   "foreground");
        addRow ("SEPARATOR",    "separator");
        addRow ("GRID LINE",    "gridLine");

        // Interactive colours
        addRow ("ACCENT",       "accent");
        addRow ("WAVEFORM",     "waveform");
        addRow ("BUTTON",       "button");
        addRow ("BUTTON HOVER", "buttonHover");
        addRow ("SELECTION",    "selectionOverlay");
        addRow ("LOCK ON",      "lockActive");
        addRow ("LOCK OFF",     "lockInactive");

        // Slice palette (16 swatches, labelled 01-16)
        for (int i = 1; i <= 16; ++i)
            addRow ("SLICE " + juce::String (i).paddedLeft ('0', 2),
                    "slice" + juce::String (i));

        // ── Preview strip ─────────────────────────────────────────────────
        previewLabel.setText ("PREVIEW", juce::dontSendNotification);
        previewLabel.setFont (DysektLookAndFeel::makeFont (10.0f));
        addAndMakeVisible (previewLabel);

        // ── Action buttons ────────────────────────────────────────────────
        applyBtn.setButtonText ("APPLY");
        applyBtn.onClick = [this] { applyToLive(); };
        addAndMakeVisible (applyBtn);

        saveBtn.setButtonText ("SAVE");
        saveBtn.onClick = [this] { saveTheme(); };
        addAndMakeVisible (saveBtn);

        resetBtn.setButtonText ("RESET");
        resetBtn.onClick = [this] { loadBaseTheme(); };
        addAndMakeVisible (resetBtn);

        // Seed with whatever theme is currently active
        loadFromTheme (getTheme());
    }

    // ── Layout ────────────────────────────────────────────────────────────
    void resized() override
    {
        const auto& T = getTheme();
        auto full  = getLocalBounds();

        // Dialog box centred in the overlay
        dialogBounds = full.reduced (juce::jmax (20, (full.getWidth()  - 820) / 2),
                                     juce::jmax (20, (full.getHeight() - 640) / 2));

        auto db = dialogBounds.reduced (0);

        // Title bar
        auto titleBar = db.removeFromTop (36);
        titleLabel.setBounds (titleBar.reduced (12, 0).withTrimmedRight (36));
        closeBtn  .setBounds (titleBar.removeFromRight (36).reduced (4));

        // Toolbar (base combo + name)
        auto toolbar = db.removeFromTop (32).reduced (12, 4);
        baseLabel.setBounds (toolbar.removeFromLeft (84));
        baseCombo.setBounds (toolbar.removeFromLeft (140));
        toolbar   .removeFromLeft (20);
        nameLabel .setBounds (toolbar.removeFromLeft (50));
        nameEditor.setBounds (toolbar.removeFromLeft (160));

        // Action buttons (bottom strip)
        auto btnRow = db.removeFromBottom (44).reduced (12, 8);
        const int bw = 90, btnGap = 8;
        applyBtn.setBounds (btnRow.removeFromRight (bw));
        btnRow.removeFromRight (btnGap);
        saveBtn .setBounds (btnRow.removeFromRight (bw));
        btnRow.removeFromRight (btnGap);
        resetBtn.setBounds (btnRow.removeFromRight (bw));

        // Preview strip
        auto previewRow = db.removeFromBottom (44);
        previewLabel.setBounds (previewRow.removeFromLeft (60).reduced (4, 0));
        previewStripBounds = previewRow.reduced (8, 8);

        // Scrollable colour rows
        scrollArea = db.reduced (0, 4);

        const int rowH  = 26;
        const int gap   = 2;
        const int cols  = 2;
        const int colW  = (scrollArea.getWidth() - 12 - 8 * (cols - 1)) / cols;

        int totalRows   = (int) rows.size();
        int rowsPerCol  = (totalRows + cols - 1) / cols;
        int contentH    = rowsPerCol * (rowH + gap);

        scrollbar.setRangeLimits ({0.0, (double) juce::jmax (0, contentH - scrollArea.getHeight())});
        scrollbar.setBounds (scrollArea.removeFromRight (8));
        addAndMakeVisible (scrollbar);
        scrollbar.addListener (this);

        rowLayoutW = colW;
        rowLayoutH = rowH;
        rowLayoutGap = gap;
        rowLayoutCols = cols;
        rowLayoutPerCol = rowsPerCol;
        rowLayoutX0 = scrollArea.getX() + 6;
        rowLayoutY0 = scrollArea.getY();

        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();

        // Dim the background
        g.setColour (juce::Colours::black.withAlpha (0.65f));
        g.fillRect  (getLocalBounds());

        // Dialog background
        g.setColour (T.header);
        g.fillRoundedRectangle (dialogBounds.toFloat(), 6.0f);

        // Title bar background
        auto titleBar = dialogBounds.withHeight (36);
        g.setColour (T.darkBar.darker (0.4f));
        g.fillRoundedRectangle (titleBar.toFloat(), 6.0f);
        g.fillRect             (titleBar.withTrimmedTop (6));

        // Border
        g.setColour (T.accent.withAlpha (0.55f));
        g.drawRoundedRectangle (dialogBounds.toFloat().reduced (0.5f), 6.0f, 1.5f);

        // Section dividers
        g.setColour (T.separator.withAlpha (0.5f));
        g.drawHorizontalLine (dialogBounds.getY() + 36, (float)dialogBounds.getX(), (float)dialogBounds.getRight());
        g.drawHorizontalLine (dialogBounds.getY() + 68, (float)dialogBounds.getX(), (float)dialogBounds.getRight());

        // Preview strip
        paintPreviewStrip (g);

        // Scroll area clip
        g.setColour (T.darkBar.darker (0.3f));
        g.fillRect  (scrollArea);
    }

    // ── Mouse: close on backdrop click ───────────────────────────────────
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! dialogBounds.contains (e.getPosition()))
            if (onDismiss) onDismiss();
    }

    // Expose working copy for external read
    ThemeData currentTheme() const { return working; }

private:
    // ── Inner types ───────────────────────────────────────────────────────

    // One colour editing row
    struct ColourRow : public juce::Component,
                        public juce::ChangeListener
    {
        juce::String    key;
        juce::Colour    colour;
        juce::Label     label;
        juce::TextButton swatch;
        juce::TextEditor hexEditor;

        std::function<void(const juce::String&, juce::Colour)> onChange;

        ColourRow (const juce::String& displayName, const juce::String& fieldKey)
            : key (fieldKey)
        {
            label.setText (displayName, juce::dontSendNotification);
            label.setFont (DysektLookAndFeel::makeFont (9.5f));
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);

            swatch.setButtonText ({});
            swatch.onClick = [this] { launchColourPicker(); };
            addAndMakeVisible (swatch);

            hexEditor.setMultiLine (false);
            hexEditor.setInputRestrictions (7, "#0123456789abcdefABCDEF");
            hexEditor.setFont (DysektLookAndFeel::makeFont (9.5f));
            hexEditor.onReturnKey = [this] { commitHex(); };
            hexEditor.onFocusLost = [this] { commitHex(); };
            addAndMakeVisible (hexEditor);

            setColour (juce::Colours::black);
        }

        void setColour (juce::Colour c)
        {
            colour = c;
            swatch.setColour (juce::TextButton::buttonColourId, c);
            swatch.setColour (juce::TextButton::buttonOnColourId, c.brighter (0.2f));
            hexEditor.setText (colourToHex (c), juce::dontSendNotification);
            repaint();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (2, 2);
            label    .setBounds (area.removeFromLeft (90));
            swatch   .setBounds (area.removeFromLeft (22));
            area      .removeFromLeft (4);
            hexEditor.setBounds (area.removeFromLeft (58));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& T = getTheme();
            g.setColour (T.button.withAlpha (0.4f));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 2.0f);
        }

    private:
        static juce::String colourToHex (juce::Colour c)
        {
            return "#" + c.toDisplayString (false).substring (2).toLowerCase();
        }

        void commitHex()
        {
            auto txt = hexEditor.getText().trim();
            if (txt.length() < 7 || txt[0] != '#') return;
            auto c = juce::Colour::fromString ("FF" + txt.substring (1));
            setColour (c);
            if (onChange) onChange (key, c);
        }

        void launchColourPicker()
        {
            auto* selector = new juce::ColourSelector (
                juce::ColourSelector::showColourAtTop |
                juce::ColourSelector::showSliders     |
                juce::ColourSelector::showColourspace, 4, 8);
            selector->setName ("colour");
            selector->setCurrentColour (colour);
            selector->setSize (280, 300);
            selector->addChangeListener (this);

            juce::CallOutBox::launchAsynchronously (
                std::unique_ptr<juce::Component> (selector),
                swatch.getScreenBounds(), nullptr);
        }

        void changeListenerCallback (juce::ChangeBroadcaster* source) override
        {
            if (auto* cs = dynamic_cast<juce::ColourSelector*> (source))
            {
                setColour (cs->getCurrentColour().withAlpha (1.0f));
                if (onChange) onChange (key, colour);
            }
        }

        // ColourRow also acts as ChangeListener for the colour picker
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ColourRow)
    };

    // ── Data ──────────────────────────────────────────────────────────────
    juce::File   themesDir;
    ThemeData    working;

    juce::OwnedArray<ColourRow>   rows;
    juce::ScrollBar               scrollbar { true };

    // Layout state (set in resized, used in layoutRows)
    juce::Rectangle<int> dialogBounds, scrollArea, previewStripBounds;
    int rowLayoutW = 200, rowLayoutH = 26, rowLayoutGap = 2;
    int rowLayoutCols = 2, rowLayoutPerCol = 15;
    int rowLayoutX0 = 0, rowLayoutY0 = 0;
    double scrollOffset = 0.0;

    // Widgets
    juce::Label      titleLabel, baseLabel, nameLabel, previewLabel;
    juce::TextButton closeBtn, applyBtn, saveBtn, resetBtn;
    juce::ComboBox   baseCombo;
    juce::TextEditor nameEditor;

    // ── Helpers ───────────────────────────────────────────────────────────

    void addRow (const juce::String& display, const juce::String& key)
    {
        auto* row = rows.add (new ColourRow (display, key));
        row->onChange = [this] (const juce::String& k, juce::Colour c)
        {
            applyColourToWorking (k, c);
            repaint();           // refresh preview strip
        };
        addAndMakeVisible (*row);
    }

    void layoutRows()
    {
        const int rh  = rowLayoutH;
        const int gap = rowLayoutGap;
        const int cw  = rowLayoutW;
        const int x0  = rowLayoutX0;
        const int y0  = rowLayoutY0;
        const int col1x = x0 + cw + 8;
        int scroll = (int) scrollOffset;

        for (int i = 0; i < rows.size(); ++i)
        {
            int col = i / rowLayoutPerCol;
            int row = i % rowLayoutPerCol;
            int x   = (col == 0) ? x0 : col1x;
            int y   = y0 + row * (rh + gap) - (col == 0 ? scroll : 0);
            rows[i]->setBounds (x, y, cw, rh);
            rows[i]->setVisible (scrollArea.intersects (rows[i]->getBounds()));
        }
    }

    // Scrollbar listener (inner class stub — handled via lambda in resized)
    void scrollBarMoved (juce::ScrollBar* bar, double newRange) override
    {
        scrollOffset = bar->getCurrentRangeStart();
        layoutRows();
    }

    // ── Populate base theme dropdown from disk ────────────────────────────
    void populateBaseCombo()
    {
        int id = 2;
        for (auto& f : themesDir.findChildFiles (juce::File::findFiles, false, "*.dsk"))
        {
            auto t = ThemeData::fromThemeFile (f.loadFileAsString());
            if (t.name.isNotEmpty())
                baseCombo.addItem (t.name, id++);
        }
    }

    void loadBaseTheme()
    {
        int sel = baseCombo.getSelectedId();
        if (sel == 1) { loadFromTheme (getTheme()); return; }

        auto text = baseCombo.getText();
        for (auto& f : themesDir.findChildFiles (juce::File::findFiles, false, "*.dsk"))
        {
            auto t = ThemeData::fromThemeFile (f.loadFileAsString());
            if (t.name == text) { loadFromTheme (t); return; }
        }
    }

    // ── Sync UI rows from a ThemeData ────────────────────────────────────
    void loadFromTheme (const ThemeData& t)
    {
        working = t;
        nameEditor.setText (t.name.isNotEmpty() ? t.name : "my-theme",
                            juce::dontSendNotification);

        auto set = [&] (const juce::String& key, juce::Colour c)
        {
            for (auto* r : rows)
                if (r->key == key) { r->setColour (c); return; }
        };

        set ("background",     t.background);
        set ("waveformBg",     t.waveformBg);
        set ("darkBar",        t.darkBar);
        set ("header",         t.header);
        set ("foreground",     t.foreground);
        set ("separator",      t.separator);
        set ("gridLine",       t.gridLine);
        set ("accent",         t.accent);
        set ("waveform",       t.waveform);
        set ("button",         t.button);
        set ("buttonHover",    t.buttonHover);
        set ("selectionOverlay", t.selectionOverlay);
        set ("lockActive",     t.lockActive);
        set ("lockInactive",   t.lockInactive);

        for (int i = 0; i < 16; ++i)
            set ("slice" + juce::String (i + 1), t.slicePalette[i]);

        repaint();
    }

    // ── Write a changed colour back into working copy ─────────────────────
    void applyColourToWorking (const juce::String& key, juce::Colour c)
    {
        if      (key == "background")      working.background      = c;
        else if (key == "waveformBg")      working.waveformBg      = c;
        else if (key == "darkBar")         working.darkBar         = c;
        else if (key == "header")          working.header          = c;
        else if (key == "foreground")      working.foreground      = c;
        else if (key == "separator")       working.separator       = c;
        else if (key == "gridLine")        working.gridLine        = c;
        else if (key == "accent")          working.accent          = c;
        else if (key == "waveform")        working.waveform        = c;
        else if (key == "button")          working.button          = c;
        else if (key == "buttonHover")     working.buttonHover     = c;
        else if (key == "selectionOverlay") working.selectionOverlay = c;
        else if (key == "lockActive")      working.lockActive      = c;
        else if (key == "lockInactive")    working.lockInactive    = c;
        else
        {
            // slice1..slice16
            auto numStr = key.fromFirstOccurrenceOf ("slice", false, false);
            int idx = numStr.getIntValue() - 1;
            if (idx >= 0 && idx < 16)
                working.slicePalette[idx] = c;
        }
    }

    // ── Apply preview to the live theme (without saving) ─────────────────
    void applyToLive()
    {
        working.name = nameEditor.getText().trim();
        if (working.name.isEmpty()) working.name = "my-theme";
        setTheme (working);
        if (onThemeChanged) onThemeChanged (working);
    }

    // ── Save as .dsk file ─────────────────────────────────────────────────
    void saveTheme()
    {
        working.name = nameEditor.getText().trim();
        if (working.name.isEmpty()) { working.name = "my-theme"; }

        // Strip characters unsafe in filenames
        auto safeName = working.name.replaceCharacters (" /\\:*?\"<>|", "---------");

        themesDir.createDirectory();
        auto file = themesDir.getChildFile (safeName + ".dsk");
        file.replaceWithText (working.toThemeFile());

        applyToLive();
        if (onThemeSaved) onThemeSaved (working.name);

        // Brief visual feedback on the save button
        saveBtn.setButtonText ("SAVED!");
        juce::Timer::callAfterDelay (1200, [this] { saveBtn.setButtonText ("SAVE"); });
    }

    // ── Preview strip (mini palette bar + bg swatch) ──────────────────────
    void paintPreviewStrip (juce::Graphics& g)
    {
        if (previewStripBounds.isEmpty()) return;

        auto strip = previewStripBounds;

        // Background swatch
        auto bgSwatch = strip.removeFromLeft (48);
        g.setColour (working.background);
        g.fillRoundedRectangle (bgSwatch.toFloat(), 3.0f);
        g.setColour (working.accent.withAlpha (0.5f));
        g.drawRoundedRectangle (bgSwatch.toFloat().reduced (0.5f), 3.0f, 1.0f);
        g.setFont (DysektLookAndFeel::makeFont (8.0f));
        g.setColour (working.foreground.withAlpha (0.7f));
        g.drawText ("BG", bgSwatch, juce::Justification::centred, false);

        strip.removeFromLeft (6);

        // Accent bar
        auto accentBar = strip.removeFromLeft (48);
        g.setColour (working.accent);
        g.fillRoundedRectangle (accentBar.toFloat(), 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.setFont (DysektLookAndFeel::makeFont (8.0f));
        g.drawText ("ACCENT", accentBar, juce::Justification::centred, false);

        strip.removeFromLeft (6);

        // 16 slice swatches
        int paletteSize = 16;
        const float sw = juce::jmin (20.0f, (float) strip.getWidth() / juce::jmax (1, paletteSize));
        float x = (float) strip.getX();
        for (int i = 0; i < paletteSize && i < 16; ++i)
        {
            juce::Rectangle<float> r (x, (float) strip.getY(), sw - 2.0f, (float) strip.getHeight());
            g.setColour (working.slicePalette[i]);
            g.fillRoundedRectangle (r, 2.0f);
            x += sw;
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeEditorPanel)
};
