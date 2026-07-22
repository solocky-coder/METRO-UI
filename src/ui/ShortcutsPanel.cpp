#include "ShortcutsPanel.h"
#include <BinaryData.h>
#include "DysektLookAndFeel.h"
#include "../PluginProcessor.h"
#include "MidiLearnDialog.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

ShortcutsPanel::ShortcutsPanel (DysektProcessor& proc)
    : processor (proc)
{
    buildShortcutData();

    // ── Title label ───────────────────────────────────────────────────────────
    titleLabel.setText ("Settings & Shortcuts", juce::dontSendNotification);
    titleLabel.setFont (DysektLookAndFeel::makeFont (19.0f, true));
    titleLabel.setColour (juce::Label::textColourId, getTheme().foreground);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    // ── Close button ──────────────────────────────────────────────────────────
    closeBtn.setColour (juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
    closeBtn.setColour (juce::TextButton::textColourOffId, getTheme().foreground.withAlpha (0.75f));
    closeBtn.onClick = [this] { if (onDismiss) onDismiss(); };
    addAndMakeVisible (closeBtn);

    // ── Theme button ──────────────────────────────────────────────────────────
    themeBtn.setColour (juce::TextButton::buttonColourId,  getTheme().button);
    themeBtn.setColour (juce::TextButton::textColourOffId, getTheme().foreground);
    themeBtn.setTooltip ("Open the theme colour editor");
    themeBtn.onClick = [this] { if (onThemeRequest) onThemeRequest(); };
    addAndMakeVisible (themeBtn);

    // ── Search box ────────────────────────────────────────────────────────────
    searchBox.setTextToShowWhenEmpty ("Search shortcuts...", getTheme().foreground.withAlpha (0.4f));
    searchBox.setFont (DysektLookAndFeel::makeFont (14.0f));
    searchBox.setColour (juce::TextEditor::backgroundColourId, getTheme().background.withAlpha (0.6f));
    searchBox.setColour (juce::TextEditor::textColourId,       getTheme().foreground);
    searchBox.setColour (juce::TextEditor::outlineColourId,    getTheme().accent.withAlpha (0.4f));
    searchBox.onTextChange = [this]
    {
        currentFilter = searchBox.getText().toLowerCase();
        shortcutsScrollY = 0;
        repaint();
    };
    addAndMakeVisible (searchBox);

    // ── RTFM button ───────────────────────────────────────────────────────────
    rtfmBtn.setColour (juce::TextButton::buttonColourId,  getTheme().button);
    rtfmBtn.setColour (juce::TextButton::textColourOffId, getTheme().accent);
    rtfmBtn.setTooltip ("Open the user manual PDF");
    rtfmBtn.onClick = [this] { openManualPdf(); };
    addAndMakeVisible (rtfmBtn);

    setWantsKeyboardFocus (true);
}

ShortcutsPanel::~ShortcutsPanel() = default;

// ─────────────────────────────────────────────────────────────────────────────
// openManualPdf
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::openManualPdf()
{
    juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("DYSEKT_User_Manual.pdf");

    if (! tmp.existsAsFile())
        tmp.replaceWithData (BinaryData::DYSEKT_User_Manual_pdf,
                             BinaryData::DYSEKT_User_Manual_pdfSize);

    if (! tmp.startAsProcess())
    {
        juce::URL ("https://github.com/solocky-coder/DYSEKT-2/releases")
            .launchInDefaultBrowser();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// buildShortcutData
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::buildShortcutData()
{
    categories.clear();

    categories.push_back ({ "Slicer & General", {
        { "Double-click",        "Add a slice at the pointer" },
        { "L",                   "Start / stop MIDI Slice" },
        { "Delete",              "Delete the selected slice" },
        { "Left / Right",        "Select previous / next slice" },
        { "Ctrl+Z",              "Undo slicer edit" },
        { "Ctrl+Shift+Z",        "Redo slicer edit" },
        { "F",                   "Toggle MIDI Follow" },
        { "M",                   "Open / close MIDI Learn" },
        { "?  (QWERTZ: Shift+ß)", "Open / close Settings" },
        { "Esc",                 "Close Settings or Piano Roll" },
    }});

    categories.push_back ({ "Piano Roll", {
        { "S / D / E",          "Select / Draw / Erase tool" },
        { "K / G",              "Split / Glue tool" },
        { "Delete / Backspace", "Delete selected notes" },
        { "Ctrl+A",             "Select all notes" },
        { "Ctrl+C / Ctrl+V",    "Copy / paste notes" },
        { "Ctrl+D",             "Duplicate selected notes" },
        { "Ctrl+Z",             "Undo Piano Roll edit" },
        { "Ctrl+Y",             "Redo Piano Roll edit" },
        { "Ctrl+Shift+Z",       "Alternate redo" },
        { "Ctrl+Q",             "Quantise selected notes" },
        { "Ctrl+0",             "Zoom to fit clip" },
        { "+ / = / -",          "Increase / decrease row height" },
        { "Ctrl+Wheel",         "Horizontal zoom" },
        { "Shift+Wheel",        "Horizontal scroll" },
        { "Wheel",              "Vertical scroll" },
    }});

    categories.push_back ({ "Arrange View", {
        { "Space",              "Play / stop" },
        { "Home / Num 0",       "Rewind and show bar 1" },
        { "+ / = / -",          "Increase / decrease track height" },
        { "Delete / Backspace", "Clear selected clip contents" },
        { "Ctrl+Wheel",         "Horizontal zoom" },
        { "Shift+Wheel",        "Fast horizontal scroll" },
        { "Alt+Wheel",          "Vertical track scroll" },
        { "Wheel",              "Horizontal scroll" },
    }});

    categories.push_back ({ "Piano Roll Mouse", {
        { "Shift+Click",        "Add / remove note from selection" },
        { "Right-click notes",  "Open note actions menu" },
        { "Drag ruler left",    "Set loop start" },
        { "Drag ruler right",   "Set loop end" },
        { "Right-click ruler",  "Clear loop region" },
    }});

    categories.push_back ({ "Arrange Mouse", {
        { "Click / drag ruler", "Seek / scrub playhead" },
        { "Alt+drag ruler",     "Set loop range" },
        { "Right-click ruler",  "Clear loop range" },
        { "Click / drag clip",  "Select / move clip" },
        { "Drag clip right edge", "Resize clip length" },
        { "Double-click clip",  "Open Piano Roll" },
        { "Right-click clip",   "Open clip menu" },
        { "Right-click empty",  "Open track menu" },
    }});

    categories.push_back ({ "Panels", {
        { "Esc",                "Close Theme Editor or message" },
        { "Return",             "Dismiss message" },
    }});
}

// ─────────────────────────────────────────────────────────────────────────────
// Event handlers
// ─────────────────────────────────────────────────────────────────────────────

bool ShortcutsPanel::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        if (onDismiss) onDismiss();
        return true;
    }
    return false;
}

void ShortcutsPanel::mouseDown (const juce::MouseEvent& e)
{
    // ── Trim preference ───────────────────────────────────────────────────────
    const int pref    = processor.trimPreference.load (std::memory_order_relaxed);
    int       newPref = pref;

    if (trimAlwaysRect.contains (e.getPosition()))
        newPref = DysektProcessor::TrimPrefAlways;
    else if (trimNeverRect.contains (e.getPosition()))
        newPref = DysektProcessor::TrimPrefNever;
    else if (trimLongRect.contains (e.getPosition()))
        newPref = DysektProcessor::TrimPrefAsk;

    if (newPref != pref)
    {
        processor.trimPreference.store (newPref, std::memory_order_relaxed);
        repaint();
        return;
    }

    // ── Interface mode ────────────────────────────────────────────────────────
    int newMode = currentUiMode;

    if (uiModeWaveRect.contains (e.getPosition()))
        newMode = 0;
    else if (uiModeGridRect.contains (e.getPosition()))
        newMode = 1;

    if (newMode != currentUiMode)
    {
        currentUiMode = newMode;
        repaint();
        if (onUiModeChanged)
            onUiModeChanged (currentUiMode);
    }
}

void ShortcutsPanel::mouseWheelMove (const juce::MouseEvent& e,
                                           const juce::MouseWheelDetails& wheel)
{
    if (! shortcutsViewport.contains (e.getPosition()) || shortcutsMaxScrollY <= 0)
        return;

    shortcutsScrollY = juce::jlimit (0, shortcutsMaxScrollY,
                                     shortcutsScrollY - juce::roundToInt (wheel.deltaY * 96.0f));
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// drawTrimPrefsSection
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::drawTrimPrefsSection (juce::Graphics& g, juce::Rectangle<int>& area)
{
    const int pref  = processor.trimPreference.load (std::memory_order_relaxed);
    const int rowH  = 28;
    const int btnH  = 22;
    const int gap   = 7;

    g.setFont (DysektLookAndFeel::makeFont (13.5f, true));
    g.setColour (getTheme().accent);
    g.drawText ("TRIM ON LOAD", area.removeFromTop (rowH), juce::Justification::centredLeft);
    area.removeFromTop (2);

    struct Option { juce::String label; int value; juce::Rectangle<int>* rect; };
    Option opts[] = {
        { "Always Trim (Default)", DysektProcessor::TrimPrefAlways, &trimAlwaysRect },
        { "Trim Long Samples",     DysektProcessor::TrimPrefAsk,    &trimLongRect   },
        { "Never Trim",            DysektProcessor::TrimPrefNever,  &trimNeverRect  },
    };

    for (auto& opt : opts)
    {
        auto row = area.removeFromTop (btnH);
        area.removeFromTop (gap);

        const bool active = (pref == opt.value);
        *opt.rect = row;

        const int dotR = 5;
        auto dotArea = row.removeFromLeft (dotR * 2 + 6);
        juce::Rectangle<float> dot (dotArea.getX() + 2.0f,
                                    dotArea.getCentreY() - (float) dotR,
                                    (float) dotR * 2.0f, (float) dotR * 2.0f);

        g.setColour (active ? getTheme().accent : getTheme().button);
        g.fillEllipse (dot);
        g.setColour (getTheme().accent.withAlpha (active ? 1.0f : 0.35f));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
        if (active)
        {
            g.setColour (getTheme().header);
            g.fillEllipse (dot.reduced (3.0f));
        }

        g.setFont (DysektLookAndFeel::makeFont (13.0f));
        g.setColour (active ? getTheme().foreground : getTheme().foreground.withAlpha (0.6f));
        g.drawText (opt.label, row.removeFromLeft (200), juce::Justification::centredLeft);
    }

    area.removeFromTop (4);
}

// ─────────────────────────────────────────────────────────────────────────────
// drawInterfaceSection
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::drawInterfaceSection (juce::Graphics& g, juce::Rectangle<int>& area)
{
    const int rowH = 28;
    const int btnH = 22;
    const int gap  = 7;

    g.setFont (DysektLookAndFeel::makeFont (13.5f, true));
    g.setColour (getTheme().accent);
    g.drawText ("INTERFACE", area.removeFromTop (rowH), juce::Justification::centredLeft);
    area.removeFromTop (2);

    struct Option { juce::String label; int value; juce::Rectangle<int>* rect; };
    Option opts[] = {
        { "Waveform View", 0, &uiModeWaveRect },
        { "Pad Grid",      1, &uiModeGridRect },
    };

    for (auto& opt : opts)
    {
        auto row = area.removeFromTop (btnH);
        area.removeFromTop (gap);

        const bool active = (currentUiMode == opt.value);
        *opt.rect = row;

        const int dotR = 5;
        auto dotArea = row.removeFromLeft (dotR * 2 + 6);
        juce::Rectangle<float> dot (dotArea.getX() + 2.0f,
                                    dotArea.getCentreY() - (float) dotR,
                                    (float) dotR * 2.0f, (float) dotR * 2.0f);

        g.setColour (active ? getTheme().accent : getTheme().button);
        g.fillEllipse (dot);
        g.setColour (getTheme().accent.withAlpha (active ? 1.0f : 0.35f));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
        if (active)
        {
            g.setColour (getTheme().header);
            g.fillEllipse (dot.reduced (3.0f));
        }

        g.setFont (DysektLookAndFeel::makeFont (13.0f));
        g.setColour (active ? getTheme().foreground : getTheme().foreground.withAlpha (0.6f));
        g.drawText (opt.label, row.removeFromLeft (200), juce::Justification::centredLeft);
    }

    area.removeFromTop (4);
}

// ─────────────────────────────────────────────────────────────────────────────
// paint
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));

    auto panel = getLocalBounds().reduced (24, 20);
    const bool metro = (getTheme().name == "metro");
    g.setColour (getTheme().header);
    g.fillRect (panel);
    g.setColour (metro ? getTheme().separator : getTheme().accent.withAlpha (0.5f));
    g.drawRect (panel, 1);

    auto content = panel.reduced (20, 14);
    content.removeFromTop (38 + 10 + 32 + 12);

    // Keep the actual preferences prominent and separate from the reference.
    auto preferences = content.removeFromTop (126);
    auto trimArea = preferences.removeFromLeft (preferences.getWidth() / 2).reduced (0, 0);
    preferences.removeFromLeft (18);
    drawTrimPrefsSection (g, trimArea);
    drawInterfaceSection (g, preferences);
    content.removeFromTop (8);
    g.setColour (getTheme().separator.withAlpha (0.55f));
    g.drawHorizontalLine (content.getY(), (float) content.getX(), (float) content.getRight());
    content.removeFromTop (10);

    shortcutsViewport = content;
    const int rowH = 26, catGap = 12, titleH = 24;
    const auto matches = [this] (const ShortcutCategory& cat)
    {
        if (currentFilter.isEmpty()) return true;
        for (const auto& entry : cat.entries)
            if (entry.keys.toLowerCase().contains (currentFilter)
                || entry.description.toLowerCase().contains (currentFilter))
                return true;
        return false;
    };
    const auto visibleRows = [this] (const ShortcutCategory& cat)
    {
        if (currentFilter.isEmpty()) return (int) cat.entries.size();
        int count = 0;
        for (const auto& entry : cat.entries)
            if (entry.keys.toLowerCase().contains (currentFilter)
                || entry.description.toLowerCase().contains (currentFilter))
                ++count;
        return count;
    };

    std::vector<const ShortcutCategory*> columns[3];
    int heights[3] = { 0, 0, 0 };
    for (const auto& cat : categories)
    {
        if (! matches (cat)) continue;
        int target = 0;
        if (heights[1] < heights[target]) target = 1;
        if (heights[2] < heights[target]) target = 2;
        columns[target].push_back (&cat);
        heights[target] += titleH + 3 + visibleRows (cat) * rowH + catGap;
    }

    shortcutsMaxScrollY = juce::jmax (0, juce::jmax (heights[0], juce::jmax (heights[1], heights[2]))
                                         - shortcutsViewport.getHeight());
    shortcutsScrollY = juce::jlimit (0, shortcutsMaxScrollY, shortcutsScrollY);

    g.saveState();
    g.reduceClipRegion (shortcutsViewport);
    const int colGap = 16;
    const int colW = (shortcutsViewport.getWidth() - colGap * 2) / 3;
    const juce::Font titleFont = DysektLookAndFeel::makeFont (13.5f, true);
    const juce::Font keyFont = DysektLookAndFeel::makeFont (12.5f, true);
    const juce::Font descriptionFont = DysektLookAndFeel::makeFont (13.0f);

    for (int c = 0; c < 3; ++c)
    {
        auto col = juce::Rectangle<int> (shortcutsViewport.getX() + c * (colW + colGap),
                                         shortcutsViewport.getY() - shortcutsScrollY,
                                         colW, heights[c]);
        for (const auto* cat : columns[c])
        {
            g.setFont (titleFont);
            g.setColour (getTheme().accent);
            g.drawText (cat->title.toUpperCase(), col.removeFromTop (titleH),
                        juce::Justification::centredLeft);
            col.removeFromTop (3);

            for (const auto& entry : cat->entries)
            {
                if (! currentFilter.isEmpty()
                    && ! entry.keys.toLowerCase().contains (currentFilter)
                    && ! entry.description.toLowerCase().contains (currentFilter))
                    continue;

                auto row = col.removeFromTop (rowH);
                const int keyW = juce::jlimit (72, 162,
                    juce::GlyphArrangement::getStringWidthInt (keyFont, entry.keys) + 18);
                auto keyRect = row.removeFromLeft (keyW);
                g.setColour (getTheme().button.withAlpha (0.95f));
                g.fillRect (keyRect.reduced (0, 2));
                g.setFont (keyFont);
                g.setColour (getTheme().accent);
                g.drawText (entry.keys, keyRect, juce::Justification::centred);
                row.removeFromLeft (8);
                g.setFont (descriptionFont);
                g.setColour (getTheme().foreground.withAlpha (0.96f));
                g.drawFittedText (entry.description, row, juce::Justification::centredLeft, 2);
            }
            col.removeFromTop (catGap);
        }
    }
    g.restoreState();

    if (shortcutsMaxScrollY > 0)
    {
        auto bar = shortcutsViewport.removeFromRight (4).reduced (0, 2);
        const int thumbH = juce::jmax (24, bar.getHeight() * bar.getHeight()
                                            / (bar.getHeight() + shortcutsMaxScrollY));
        const int thumbY = bar.getY() + (bar.getHeight() - thumbH) * shortcutsScrollY / shortcutsMaxScrollY;
        g.setColour (getTheme().separator.withAlpha (0.65f));
        g.fillRect (bar);
        g.setColour (getTheme().accent.withAlpha (0.85f));
        g.fillRect (bar.withY (thumbY).withHeight (thumbH));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resized
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPanel::resized()
{
    auto panel  = getLocalBounds().reduced (24, 20);
    auto header = panel.reduced (20, 14);

    auto titleRow = header.removeFromTop (38);
    closeBtn .setBounds (titleRow.removeFromRight (38));
    themeBtn .setBounds (titleRow.removeFromRight (154));
    titleRow.removeFromRight (10);
    rtfmBtn  .setBounds (titleRow.removeFromRight (62));
    titleRow.removeFromRight (10);
    titleLabel.setBounds (titleRow);
    header.removeFromTop (10);
    searchBox.setBounds (header.removeFromTop (32));
}
