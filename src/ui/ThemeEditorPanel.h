#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <cmath>
#include "DysektLookAndFeel.h"

// ============================================================
//  ThemeEditorPanel
//  Dockable live-preview rail. Lets the user create, modify
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
        setWantsKeyboardFocus (true);
        scrollbar.addListener (this);
        addAndMakeVisible (scrollbar);

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
        nameEditor.onReturnKey = [this] { liveApply(); };
        nameEditor.onFocusLost = [this] { liveApply(); };
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

        // Slice palette — one swatch per pad across both 16-pad banks.
        for (int i = 1; i <= ThemeData::kSlicePaletteSize; ++i)
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
        juce::ignoreUnused (T);
        dialogBounds = getLocalBounds().reduced (4, 8);
        auto db = dialogBounds;

        auto titleBar = db.removeFromTop (36);
        titleLabel.setBounds (titleBar.reduced (12, 0).withTrimmedRight (36));
        closeBtn.setBounds (titleBar.removeFromRight (36).reduced (4));

        // A rail is too narrow for the old one-line toolbar, so stack its fields.
        auto toolbar = db.removeFromTop (58).reduced (12, 4);
        auto baseRow = toolbar.removeFromTop (24);
        baseLabel.setBounds (baseRow.removeFromLeft (66));
        baseCombo.setBounds (baseRow);
        auto nameRow = toolbar;
        nameLabel.setBounds (nameRow.removeFromLeft (42));
        nameEditor.setBounds (nameRow);

        auto btnRow = db.removeFromBottom (42).reduced (12, 7);
        constexpr int buttonGap = 6;
        const int buttonW = (btnRow.getWidth() - buttonGap * 3) / 4;
        resetBtn.setBounds (btnRow.removeFromLeft (buttonW));
        btnRow.removeFromLeft (buttonGap);
        saveBtn.setBounds (btnRow.removeFromLeft (buttonW));
        btnRow.removeFromLeft (buttonGap);
        applyBtn.setBounds (btnRow.removeFromLeft (buttonW));
        btnRow.removeFromLeft (buttonGap);
        pickBtn.setBounds (btnRow);

        auto previewRow = db.removeFromBottom (122).reduced (8, 6);
        previewLabel.setBounds (previewRow.removeFromTop (16));
        previewStripBounds = previewRow.reduced (0, 2);

        scrollArea = db.reduced (6, 4);
        const int rowH = 26, gap = 2;
        const int cols = dialogBounds.getWidth() > 500 ? 2 : 1;
        const int colW = (scrollArea.getWidth() - 8 - 8 * (cols - 1)) / cols;
        const int totalRows = (int) rows.size();
        const int rowsPerCol = (totalRows + cols - 1) / cols;
        const int contentH = rowsPerCol * (rowH + gap);

        scrollbar.setRangeLimits ({ 0.0, (double) juce::jmax (0, contentH - scrollArea.getHeight()) });
        scrollbar.setBounds (scrollArea.removeFromRight (8));

        rowLayoutW = colW;
        rowLayoutH = rowH;
        rowLayoutGap = gap;
        rowLayoutCols = cols;
        rowLayoutPerCol = rowsPerCol;
        rowLayoutX0 = scrollArea.getX() + 2;
        rowLayoutY0 = scrollArea.getY();
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();
        g.setColour (T.header);
        g.fillRect (dialogBounds);

        auto titleBar = dialogBounds.withHeight (36);
        g.setColour (T.darkBar.darker (0.4f));
        g.fillRect (titleBar);

        g.setColour (T.accent.withAlpha (0.55f));
        g.drawRect (dialogBounds, 1);
        g.setColour (T.separator.withAlpha (0.5f));
        g.drawHorizontalLine (dialogBounds.getY() + 36, (float) dialogBounds.getX(), (float) dialogBounds.getRight());
        g.drawHorizontalLine (dialogBounds.getY() + 94, (float) dialogBounds.getX(), (float) dialogBounds.getRight());

        paintPreviewStrip (g);
        g.setColour (T.darkBar.darker (0.3f));
        g.fillRect (scrollArea);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onDismiss) onDismiss();
            return true;
        }
        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (pickModeActive && previewStripBounds.contains (e.getPosition()))
            pickPreviewTarget (e.getPosition());
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

        void openPicker() { launchColourPicker(); }

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
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 0.0f);
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
    juce::TextButton closeBtn, applyBtn, saveBtn, resetBtn, pickBtn;
    bool pickModeActive = false;
    juce::ComboBox   baseCombo;
    juce::TextEditor nameEditor;

    // ── Helpers ───────────────────────────────────────────────────────────

    void addRow (const juce::String& display, const juce::String& key)
    {
        auto* row = rows.add (new ColourRow (display, key));
        row->onChange = [this] (const juce::String& k, juce::Colour c)
        {
            applyColourToWorking (k, c);
            liveApply();
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

        for (int i = 0; i < ThemeData::kSlicePaletteSize; ++i)
            set ("slice" + juce::String (i + 1), t.slicePalette[i]);

        liveApply();
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
            // slice1..slice32
            auto numStr = key.fromFirstOccurrenceOf ("slice", false, false);
            int idx = numStr.getIntValue() - 1;
            if (idx >= 0 && idx < ThemeData::kSlicePaletteSize)
                working.slicePalette[idx] = c;
        }
    }

    // ── Apply preview to the live theme (without saving) ─────────────────
    void liveApply()
    {
        working.name = nameEditor.getText().trim();
        if (working.name.isEmpty()) working.name = "my-theme";
        setTheme (working);
        if (onThemeChanged) onThemeChanged (working);
        repaint();
    }

    void applyToLive() { liveApply(); }

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

    // ── Miniature widget preview ──────────────────────────────────────────
    void paintPreviewStrip (juce::Graphics& g)
    {
        if (previewStripBounds.isEmpty()) return;
        auto area = previewStripBounds;
        g.setColour (working.background);
        g.fillRect (area);
        g.setColour (working.separator.withAlpha (0.7f));
        g.drawRect (area, 1);

        // Waveform stamp: waveform background, grid and signal line.
        auto wave = area.removeFromTop (42).reduced (4, 3);
        g.setColour (working.waveformBg);
        g.fillRect (wave);
        g.setColour (working.gridLine.withAlpha (0.8f));
        for (int x = wave.getX() + 8; x < wave.getRight(); x += 12) g.drawVerticalLine (x, (float) wave.getY(), (float) wave.getBottom());
        g.drawHorizontalLine (wave.getCentreY(), (float) wave.getX(), (float) wave.getRight());
        juce::Path signal;
        signal.startNewSubPath ((float) wave.getX(), (float) wave.getCentreY());
        for (int x = wave.getX(); x < wave.getRight(); x += 3)
            signal.lineTo ((float) x, (float) wave.getCentreY() + std::sin ((float) (x - wave.getX()) * 0.22f) * wave.getHeight() * 0.31f);
        g.setColour (working.waveform);
        g.strokePath (signal, juce::PathStrokeType (1.5f));

        auto bottom = area.reduced (4, 2);
        auto knob = bottom.removeFromLeft (42).reduced (2);
        auto knobBounds = knob.toFloat();
        auto centre = knobBounds.getCentre();
        auto radius = juce::jmin (knobBounds.getWidth(), knobBounds.getHeight()) * 0.38f;
        g.setColour (working.darkBar);
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, 2.35f, 5.20f, true);
        g.setColour (working.accent);
        g.strokePath (arc, juce::PathStrokeType (2.5f));
        g.setColour (working.foreground);
        g.drawLine (centre.x, centre.y, centre.x + radius * 0.55f, centre.y - radius * 0.38f, 1.5f);

        auto button = bottom.removeFromLeft (56).reduced (2, 5);
        g.setColour (working.button);
        g.fillRect (button);
        g.setColour (working.lockActive);
        g.drawRect (button, 1);
        g.setColour (working.foreground);
        g.setFont (DysektLookAndFeel::makeFont (8.0f, true));
        g.drawText ("LOCK", button, juce::Justification::centred, false);

        // Two 16-pad banks: slices 01–16 above 17–32.
        const auto paletteArea = bottom.reduced (0, 2);
        const int bankH = juce::jmax (3, paletteArea.getHeight() / 2);
        const int swatchW = juce::jmax (4, paletteArea.getWidth() / 16);
        for (int i = 0; i < ThemeData::kSlicePaletteSize; ++i)
        {
            const int bank = i / 16;
            const int column = i % 16;
            auto swatch = juce::Rectangle<int> (paletteArea.getX() + column * swatchW,
                                                 paletteArea.getY() + bank * bankH,
                                                 swatchW - 1, bankH - 1);
            g.setColour (working.slicePalette[i]);
            g.fillRect (swatch);
        }

        if (pickModeActive)
        {
            g.setColour (working.accent.withAlpha (0.8f));
            g.drawRect (previewStripBounds, 2);
        }
    }

    void pickPreviewTarget (juce::Point<int> point)
    {
        auto area = previewStripBounds;
        const auto wave = area.removeFromTop (42).reduced (4, 3);
        juce::String key = "accent";
        if (wave.contains (point))
            key = point.getY() < wave.getCentreY() - 4 ? "waveformBg" : "waveform";
        else
        {
            auto bottom = area.reduced (4, 2);
            const auto knob = bottom.removeFromLeft (42);
            const auto button = bottom.removeFromLeft (56);
            if (button.contains (point)) key = "lockActive";
            else if (! knob.contains (point))
            {
                const auto paletteArea = bottom.reduced (0, 2);
                const int bankH = juce::jmax (1, paletteArea.getHeight() / 2);
                const int bank = juce::jlimit (0, 1, (point.y - paletteArea.getY()) / bankH);
                const int column = juce::jlimit (0, 15, (point.x - paletteArea.getX()) * 16
                                                      / juce::jmax (1, paletteArea.getWidth()));
                key = "slice" + juce::String (bank * 16 + column + 1);
            }
        }
        for (auto* row : rows)
            if (row->key == key) { row->openPicker(); break; }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeEditorPanel)
};
