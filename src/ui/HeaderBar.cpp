#include "HeaderBar.h"
#include "UIHelpers.h"
#include "DysektLookAndFeel.h"
#include "../PluginProcessor.h"
#include "../PluginEditor.h"
#include "../audio/GrainEngine.h"
#include "BinaryData.h"

HeaderBar::HeaderBar (DysektProcessor& p)
 : processor (p),
   controlFrame (p)
{
    // Standard buttons
    for (auto* btn : { &undoBtn, &redoBtn, &panicBtn, &shortcutsBtn })
    {
        btn->setAlwaysOnTop (true);
        btn->setColour (juce::TextButton::buttonColourId, getTheme().button);
        btn->setColour (juce::TextButton::textColourOnId, getTheme().foreground);
        btn->setColour (juce::TextButton::textColourOffId, getTheme().foreground);
        addAndMakeVisible (*btn);
    }

    // Cogwheel icon — U+2699 GEAR, UTF-8: E2 9A 99
    shortcutsBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x9a\x99"));
    shortcutsBtn.setTooltip ("Settings");
    shortcutsBtn.onClick = [this] { if (onShortcutsToggle) onShortcutsToggle(); };


    panicBtn.setTooltip ("Panic: kill all sound");
    panicBtn.onClick = [this] {
        DysektProcessor::Command cmd;
        cmd.type = DysektProcessor::CmdPanic;
        processor.pushCommand (cmd);
    };

    undoBtn.setTooltip ("Undo (Ctrl+Z)");
    undoBtn.onClick = [this] {
        DysektProcessor::Command cmd;
        cmd.type = DysektProcessor::CmdUndo;
        processor.pushCommand (cmd);
    };

    redoBtn.setTooltip ("Redo (Ctrl+Shift+Z)");
    redoBtn.onClick = [this] {
        DysektProcessor::Command cmd;
        cmd.type = DysektProcessor::CmdRedo;
        processor.pushCommand (cmd);
    };

    // Wire control frame callbacks → forward to PluginEditor via HeaderBar's lambdas
    controlFrame.onBrowserToggle = [this] { if (onBrowserToggle) onBrowserToggle(); };
    controlFrame.onWaveToggle    = [this] { if (onWaveToggle)    onWaveToggle(); };
    controlFrame.onMidiFollowToggle = [this] { if (onMidiFollowToggle) onMidiFollowToggle(); };
    controlFrame.onBodeToggle    = [this] { if (onBodeToggle)    onBodeToggle(); };
    controlFrame.onEqToggle      = [this] { if (onEqToggle)      onEqToggle(); };
    controlFrame.onSeqToggle     = [this] { if (onSeqToggle)     onSeqToggle(); };
    // Note: controlFrame is NOT added as a visible child here —
    // PluginEditor::resized() calls addAndMakeVisible(*headerBar.getControlFrame())
    // and positions it between the two LCD panels.

    startTimerHz (10);   // polls processor.globalMidiActivity for the LED
}

void HeaderBar::timerCallback()
{
    const int cur = processor.globalMidiActivity.load (std::memory_order_relaxed);
    bool needsRepaint = false;

    if (cur != lastGlobalMidiActivity)
    {
        lastGlobalMidiActivity = cur;
        midiHoldCounter = kMidiHoldTicks;
        needsRepaint = true;
    }
    else if (midiHoldCounter > 0)
    {
        --midiHoldCounter;
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint (midiActivityDotBounds);
}

// ── State sync ────────────────────────────────────────────────────────────────

void HeaderBar::setBrowserActive  (bool v) { controlFrame.setBrowserActive (v); }
void HeaderBar::setWaveActive     (bool v) { controlFrame.setWaveActive (v); }
void HeaderBar::setWaveMode       (int  m) { controlFrame.setWaveMode (m); }
void HeaderBar::setMidiFollowActive (bool v) { controlFrame.setMidiFollowActive (v); }
void HeaderBar::setBodeActive     (bool v) { controlFrame.setBodeActive (v); }
void HeaderBar::setEqActive       (bool v) { controlFrame.setEqActive (v); }
void HeaderBar::setSeqActive      (bool v) { controlFrame.setSeqActive (v); }

// ── resized ───────────────────────────────────────────────────────────────────

void HeaderBar::resized()
{
    const int w   = getWidth();
    const int h   = getHeight();
    // Scale inset/gap relative to design-time height (kBtnBarH = 38px).
    static constexpr float kBaseH = 38.0f;
    const float sf    = (float) h / kBaseH;
    const int   pad   = juce::roundToInt (3 * sf);  // inset from the rounded frame border
    const int   gap   = juce::roundToInt (2 * sf);

    // Reserve a small strip for the global MIDI activity LED, left of UNDO —
    // lit briefly on any incoming note from any engine, not just SF2/SFZ.
    const int dotW = juce::roundToInt (14 * sf);
    midiActivityDotBounds = { pad, pad, dotW, h - pad * 2 };

    // Four equal-width buttons in a single horizontal row:
    // UNDO | REDO | PANIC | ⚙
    const int btnX0  = pad + dotW + gap;
    const int innerW = w - btnX0 - pad;
    const int btnW   = innerW / 4;

    undoBtn     .setBounds (btnX0,               pad, btnW - gap, h - pad * 2);
    redoBtn     .setBounds (btnX0 + btnW,        pad, btnW - gap, h - pad * 2);
    panicBtn    .setBounds (btnX0 + btnW * 2,    pad, btnW - gap, h - pad * 2);
    shortcutsBtn.setBounds (btnX0 + btnW * 3,    pad, w - (btnX0 + btnW * 3) - gap, h - pad * 2);
}

// ── paint ─────────────────────────────────────────────────────────────────────

void HeaderBar::paint (juce::Graphics& g)
{
    // Refresh standard button colours to follow theme
    for (auto* btn : { &undoBtn, &redoBtn, &panicBtn, &shortcutsBtn })
    {
        btn->setColour (juce::TextButton::buttonColourId, getTheme().button);
        btn->setColour (juce::TextButton::textColourOnId,  getTheme().accent);
        btn->setColour (juce::TextButton::textColourOffId, getTheme().foreground);
    }

    const auto accent = getTheme().accent;
    auto b = getLocalBounds();

    // Global MIDI activity LED — lit briefly on any incoming note, from any
    // engine (MAIN/Slicer, SF2-Player, SFZ-Player). Drawn before the
    // theme-specific branches below so it appears consistently everywhere.
    if (! midiActivityDotBounds.isEmpty())
    {
        const bool lit = midiHoldCounter > 0;
        g.setColour (lit ? juce::Colour (0xFF00FF88) : getTheme().separator.withAlpha (0.6f));
        const auto db = midiActivityDotBounds.toFloat();
        const float dotSize = juce::jmin (db.getWidth(), db.getHeight()) * 0.55f;
        g.fillEllipse (db.getCentreX() - dotSize * 0.5f, db.getCentreY() - dotSize * 0.5f,
                        dotSize, dotSize);
    }


    // Dark rounded background — metallic gradient for serum, flat for all others
    const auto& theme = getTheme();

    if (theme.name == "metro")
    {
        g.setColour (theme.waveformBg);
        g.fillRoundedRectangle (b.toFloat(), 0.0f);
        g.setColour (theme.separator);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 0.0f, 1.0f);

        sampleInfoBounds = {};
        slicesInfoArea   = {};
        (void) processor;
        return;
    }

    juce::ColourGradient bgGrad;
    if (theme.name == "serum")
    {
        // Three-stop metallic: bright steel top → dark mid → slight lift bottom
        auto base = theme.darkBar;
        bgGrad = juce::ColourGradient (base.brighter (0.22f), 0, 0,
                                       base.darker   (0.12f), 0, (float) b.getHeight(), false);
        bgGrad.addColour (0.65, base.brighter (0.08f));
    }
    else
    {
        bgGrad = juce::ColourGradient (juce::Colour (0xFF131313), 0, 0,
                                       juce::Colour (0xFF0E0E0E), 0, (float) b.getHeight(), false);
    }
    g.setGradientFill (bgGrad);
    g.fillRoundedRectangle(b.toFloat(), 0.0f);

    // Bright accent border — matches DualLcdControlFrame / LCD panels
    g.setColour (accent.withAlpha (0.60f));
    g.drawRoundedRectangle(b.toFloat().reduced (0.5f), 0.0f, 1.0f);

    sampleInfoBounds = {};
    slicesInfoArea   = {};
    (void) processor;
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void HeaderBar::mouseDown (const juce::MouseEvent& e)
{
    if (textEditor) textEditor.reset();
    const auto& ui = processor.getUiSliceSnapshot();
    auto pos = e.getPosition();

    if (sampleInfoBounds.contains (pos) && ui.sampleMissing)
        openRelinkBrowser();
}

void HeaderBar::mouseDrag       (const juce::MouseEvent&) {}
void HeaderBar::mouseUp         (const juce::MouseEvent&) {}

void HeaderBar::mouseDoubleClick (const juce::MouseEvent& /*e*/) {}

// ── Helpers ───────────────────────────────────────────────────────────────────

void HeaderBar::showThemePopup()
{
    auto* editor = dynamic_cast<DysektEditor*> (getParentComponent());
    if (editor == nullptr) return;

    auto themes      = editor->getAvailableThemes();
    auto currentName = getTheme().name;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Theme");
    for (int i = 0; i < themes.size(); ++i)
        menu.addItem (i + 1, themes[i], true, themes[i] == currentName);

    auto* topLevel = getTopLevelComponent();
    float ms2 = DysektLookAndFeel::getMenuScale();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&shortcutsBtn)
                                                  .withParentComponent (topLevel)
                                                  .withStandardItemHeight ((int) (24 * ms2)),
        [this, editor, themes] (int result) {
            if (result > 0 && result <= themes.size())
                editor->applyTheme (themes[result - 1]);
        });
}

void HeaderBar::openRelinkBrowser()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Relink Audio File",
        juce::File(),
        "*.wav;*.ogg;*.aiff;*.flac;*.mp3");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result.existsAsFile())
                processor.relinkFileAsync (result);
        });
}
