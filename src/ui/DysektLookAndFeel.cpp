#include "DysektLookAndFeel.h"
#include "BinaryData.h"
#include "IconManager.h"

// Default theme is now Metro (flat, square, no-gloss chrome per the design
// manual). Was previously ThemeData::opendawTheme(), which meant every
// theme.name == "metro" flat-rendering branch across the codebase
// (ActionPanel, FileBrowserPanel, GlobalEqPanel, HeaderBar, LogoBar,
// PadGridView, Sf2LcdDisplay, Sf2ProgramGrid, SfzDropdownPanel,
// SfzLcdDisplay, ShortcutsPanel, SliceLcdDisplay, DualLcdControlFrame,
// LcdColours) was unreachable — the app rendered the old rounded/gradient/
// glow "opendaw" look by default even though the flat code path was already
// implemented and correct. Flipping this one line activates the flat look
// everywhere at once, with no other file needing to change.
static ThemeData globalTheme = ThemeData::metroTheme();

ThemeData& getTheme() { return globalTheme; }
void setTheme (const ThemeData& t) { globalTheme = t; }

juce::Typeface::Ptr DysektLookAndFeel::sRegularTypeface;
juce::Typeface::Ptr DysektLookAndFeel::sBoldTypeface;
juce::Typeface::Ptr DysektLookAndFeel::sMonoTypeface;
juce::Typeface::Ptr DysektLookAndFeel::sMonoBoldTypeface;
float DysektLookAndFeel::sMenuScale = 1.0f;

DysektLookAndFeel::DysektLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, getTheme().background);

    // Labels / UI text — Barlow Condensed (sharp, narrow, technical)
    regularTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::BarlowCondensedRegular_ttf, BinaryData::BarlowCondensedRegular_ttfSize);
    boldTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize);

    // Values / numbers — JetBrains Mono (monospaced, digits never jump width)
    monoTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
    monoBoldTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::JetBrainsMonoBold_ttf, BinaryData::JetBrainsMonoBold_ttfSize);

    sRegularTypeface  = regularTypeface;
    sBoldTypeface     = boldTypeface;
    sMonoTypeface     = monoTypeface;
    sMonoBoldTypeface = monoBoldTypeface;
}

juce::Font DysektLookAndFeel::makeFont (float pointSize, bool bold)
{
    // Font selection is unrelated to the flat/square shape-language collapse —
    // Metro still asks for native Segoe UI chrome; every other theme still
    // uses the embedded Barlow Condensed faces. Left exactly as it was.
    if (getTheme().name == "metro")
    {
        auto opts = juce::FontOptions().withName ("Segoe UI").withHeight (pointSize);
        if (bold)
            opts = opts.withStyle ("Semibold");
        return juce::Font (opts);
    }

    auto tf = bold ? sBoldTypeface : sRegularTypeface;
    if (tf != nullptr)
        return juce::Font (juce::FontOptions().withTypeface (tf).withPointHeight (pointSize));
    return juce::Font (juce::FontOptions().withHeight (pointSize));
}

juce::Font DysektLookAndFeel::makeMonoFont (float pointSize, bool bold)
{
    auto tf = bold ? sMonoBoldTypeface : sMonoTypeface;
    if (tf != nullptr)
        return juce::Font (juce::FontOptions().withTypeface (tf).withPointHeight (pointSize));
    return makeFont (pointSize, bold);
}

juce::Typeface::Ptr DysektLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    // Check both the bold flag and the typeface style string — macOS can request
    // bold via style name ("Bold", "SemiBold") rather than the isBold() flag,
    // which would otherwise fall through to a system typeface (Helvetica, etc.)
    auto style = f.getTypefaceStyle().toLowerCase();
    if (f.isBold() || style.contains ("bold") || style.contains ("semi"))
        return boldTypeface;
    return regularTypeface;
}

// ── Buttons ───────────────────────────────────────────────────────────────────
// COLLAPSED: this used to branch metro (flat/square/accent-only) vs. every
// other theme (asset sprite base + serum metallic gradient + rounded border).
// The else branch is gone — every theme now renders the flat/square shape,
// still reading its own getTheme() colours.
void DysektLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& /*bgColour*/,
                                               bool isHighlighted, bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float r = 0.0f;   // flat, square corners — was Metro-only, now universal

    auto btnCol = button.findColour (juce::TextButton::buttonColourId);
    auto baseBg = (btnCol != juce::Colour()) ? btnCol : getTheme().button;

    if (baseBg.isTransparent())
        return;

    const bool toggled = button.getToggleState();

    auto fill = isDown        ? getTheme().accent
              : toggled       ? getTheme().accent
              : isHighlighted ? getTheme().buttonHover
                              : getTheme().button;
    g.setColour (fill);
    g.fillRoundedRectangle (bounds, r);
}

void DysektLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                         bool /*isHighlighted*/, bool /*isDown*/)
{
    auto textCol = button.findColour (button.getToggleState()
                                       ? juce::TextButton::textColourOnId
                                       : juce::TextButton::textColourOffId);
    if (textCol.isTransparent())
        textCol = button.getToggleState()
                ? getTheme().accent
                : getTheme().foreground.withAlpha (0.85f);

    g.setColour (textCol);

    const int h = button.getHeight();
    // Symbol-only button (loop): single non-ASCII char — render larger with system font
    const juce::String txt = button.getButtonText();
    const bool isSymbol = (txt.length() <= 2 && txt[0] > 127);
    float fontSize = isSymbol ? (h < 22 ? 14.0f : 17.0f)
                   : h < 16 ? 8.0f
                   : h < 22 ? 10.0f
                   : h < 28 ? 11.0f
                   : 13.0f;
    g.setFont (isSymbol ? juce::Font (fontSize) : makeFont (fontSize));
    g.drawText (button.getButtonText(), button.getLocalBounds().reduced (2, 0),
                juce::Justification::centred);
}

// ── Popup menus ───────────────────────────────────────────────────────────────
void DysektLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const float r = 0.0f;   // flat, square corners — was Metro-only, now universal
    auto bounds = juce::Rectangle<float> (0, 0, (float)width, (float)height);
    const auto bgColour = getTheme().darkBar.brighter (0.06f);

    // Fill entire rect first — eliminates white OS window corners behind rounded shape
    g.fillAll (bgColour);

    // Flat panel fill for every theme — the serum metallic-gradient special
    // case is gone, this reads plainly from getTheme() now.
    g.setColour (bgColour);
    g.fillRoundedRectangle (bounds, r);

    // Outer border — accent tint, full opacity
    g.setColour (getTheme().separator.withAlpha (1.0f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), r, 1.0f);

    // Top accent line — 1px, subtle
    g.setColour (getTheme().accent.withAlpha (0.22f));
    g.fillRect (bounds.reduced (r, 0).removeFromTop (1.0f));
}

void DysektLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                            bool isSeparator, bool isActive, bool isHighlighted,
                                            bool isTicked, bool /*hasSubMenu*/,
                                            const juce::String& text, const juce::String& /*shortcutText*/,
                                            const juce::Drawable* /*icon*/, const juce::Colour* /*textColour*/)
{
    // Solid background first
    g.setColour (getTheme().darkBar.brighter (0.06f));
    g.fillRect (area);

    if (isSeparator)
    {
        g.setColour (getTheme().separator);
        g.fillRect (area.reduced (4, 0).withHeight (1).withY (area.getCentreY()));
        return;
    }

    if (isHighlighted && isActive)
    {
        // Flat highlight — square corners now (was fixed 2.0f radius regardless
        // of theme; flattened to match the rest of the shape language).
        g.setColour (getTheme().buttonHover);
        g.fillRoundedRectangle (area.reduced (3, 1).toFloat(), 0.0f);

        // Accent left edge — 2px solid bar
        g.setColour (getTheme().accent);
        g.fillRect (juce::Rectangle<float> ((float)area.getX() + 3, (float)area.getY() + 3,
                                             2.0f, (float)area.getHeight() - 6));
    }

    const int tickZoneW = (int) (16 * sMenuScale);
    if (isTicked)
    {
        const int dotSize = (int) (4 * sMenuScale);
        g.setColour (getTheme().accent);
        g.fillRect (area.getX() + (tickZoneW - dotSize) / 2,
                    area.getCentreY() - dotSize / 2,
                    dotSize, dotSize);
    }

    const juce::Colour textCol = isTicked ? getTheme().accent
                               : isActive ? getTheme().foreground
                                          : getTheme().foreground.withAlpha (0.4f);
    g.setColour (textCol);
    g.setFont (getPopupMenuFont());

    auto textArea = area.withLeft (area.getX() + tickZoneW)
                        .withRight (area.getRight() - (int) (4 * sMenuScale));
    g.drawText (text, textArea, juce::Justification::centredLeft);
}

void DysektLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g,
                                                     const juce::Rectangle<int>& area,
                                                     const juce::String& sectionName)
{
    g.setFont (makeFont (14.0f * sMenuScale, true));
    g.setColour (getTheme().foreground);
    g.drawFittedText (sectionName,
                      area.getX() + (int) (12 * sMenuScale), area.getY(),
                      area.getWidth() - (int) (16 * sMenuScale),
                      (int) ((float) area.getHeight() * 0.8f),
                      juce::Justification::bottomLeft, 1);
}

juce::Font DysektLookAndFeel::getPopupMenuFont()
{
    return makeFont (14.0f * sMenuScale);
}

// ── ComboBox ──────────────────────────────────────────────────────────────────
// COLLAPSED: this used to branch metro (flat fill + chevron) vs. every other
// theme (bevel gradient body + glow-on-focus + chevron). The else branch is
// gone — every theme now renders the flat Metro-style combo box, still
// reading its own getTheme() colours.
void DysektLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
                                      bool isButtonDown, int buttonX, int /*buttonY*/,
                                      int /*buttonW*/, int /*buttonH*/, juce::ComboBox& box)
{
    const auto& t = getTheme();
    auto cbBounds = juce::Rectangle<float> (0, 0, (float)width, (float)height).reduced (0.5f);

    const float r = 4.0f;   // Metro combo-box radius — kept as-is, now universal
    g.setColour (isButtonDown ? t.buttonHover : t.button);
    g.fillRoundedRectangle (cbBounds, r);

    const bool focused = box.hasKeyboardFocus (false);
    g.setColour (focused ? t.accent : t.separator);
    g.drawRoundedRectangle (cbBounds, r, 1.0f);

    const int arrowCX = buttonX + (width - buttonX) / 2;
    const int arrowCY = height / 2;
    const int arrowHalf = 4;
    g.setColour (t.foreground.withAlpha (0.85f));
    g.drawLine ((float)(arrowCX - arrowHalf), (float)(arrowCY - 2),
                (float)(arrowCX),              (float)(arrowCY + 2), 1.5f);
    g.drawLine ((float)(arrowCX),              (float)(arrowCY + 2),
                (float)(arrowCX + arrowHalf),  (float)(arrowCY - 2), 1.5f);
}

juce::Font DysektLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return makeFont (14.0f * sMenuScale);
}

void DysektLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (1, 1, box.getWidth() - 28, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setColour (juce::Label::textColourId, getTheme().foreground);
    label.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

// Scroll arrows — filled triangles, no unicode
void DysektLookAndFeel::drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height,
                                                  bool isScrollUpArrow)
{
    g.setColour (getTheme().foreground.withAlpha (0.6f));
    const float cx = (float) width * 0.5f;
    const float cy = (float) height * 0.5f;
    const float sz = 4.0f;
    juce::Path arrow;
    if (isScrollUpArrow)
        arrow.addTriangle (cx - sz, cy + sz * 0.5f, cx + sz, cy + sz * 0.5f, cx, cy - sz * 0.5f);
    else
        arrow.addTriangle (cx - sz, cy - sz * 0.5f, cx + sz, cy - sz * 0.5f, cx, cy + sz * 0.5f);
    g.fillPath (arrow);
}

// ── Tooltip ───────────────────────────────────────────────────────────────────
void DysektLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    // Flattened — was a fixed 2.0f radius regardless of theme; now square
    // corners everywhere, in line with the rest of the shape language.
    const float r = 0.0f;
    auto bounds = juce::Rectangle<float> (0, 0, (float)width, (float)height);

    g.setColour (getTheme().darkBar.brighter (0.10f));
    g.fillRoundedRectangle (bounds, r);

    g.setColour (getTheme().accent.withAlpha (0.55f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), r, 1.0f);

    g.setColour (getTheme().foreground.withAlpha (0.95f));
    g.setFont (makeFont (13.0f));
    g.drawText (text, 6, 0, width - 12, height, juce::Justification::centredLeft);
}

juce::Rectangle<int> DysektLookAndFeel::getTooltipBounds (const juce::String& text,
                                                            juce::Point<int> screenPos,
                                                            juce::Rectangle<int> parentArea)
{
    int w = (int) juce::GlyphArrangement::getStringWidth(makeFont (14.0f), text) + 14;
    int h = 24;
    int x = screenPos.x;
    int y = screenPos.y + 18;

    if (x + w > parentArea.getRight())
        x = parentArea.getRight() - w;
    if (y + h > parentArea.getBottom())
        y = screenPos.y - h - 4;

    return { x, y, w, h };
}

// ── Scrollbar ─────────────────────────────────────────────────────────────────
// Flattened — was always a rounded "pill" thumb with a gradient fill and a
// hover/drag glow regardless of theme. Now a flat square-cornered accent bar,
// no gradient, no glow, matching the rest of the shape language.
void DysektLookAndFeel::drawScrollbar (juce::Graphics& g,
                                       juce::ScrollBar& /*scrollbar*/,
                                       int x, int y, int width, int height,
                                       bool isScrollbarVertical,
                                       int thumbStartPosition,
                                       int thumbSize,
                                       bool isMouseOver,
                                       bool isMouseDown)
{
    const auto& t = getTheme();

    // Track — flat, dark
    g.setColour (t.darkBar.darker (0.30f));
    g.fillRect (x, y, width, height);
    g.setColour (t.separator);
    g.drawRect (x, y, width, height, 1);

    if (thumbSize > 0)
    {
        juce::Rectangle<int> thumb;
        if (isScrollbarVertical)
            thumb = { x + 1, y + thumbStartPosition, width - 2, thumbSize };
        else
            thumb = { x + thumbStartPosition, y + 1, thumbSize, height - 2 };

        auto thumbF = thumb.toFloat().reduced (1.0f);
        const float baseAlpha = isMouseDown ? 0.85f : isMouseOver ? 0.70f : 0.55f;

        g.setColour (t.accent.withAlpha (baseAlpha));
        g.fillRect (thumbF);

        g.setColour (t.accent.withAlpha (0.40f));
        g.drawRect (thumbF, 1.0f);
    }
}

// ── Linear slider ─────────────────────────────────────────────────────────────
// COLLAPSED: this used to branch metro (thin track + accent fill + round
// dot thumb) vs. every other theme (recessed gradient track + glow fill +
// glassy beveled thumb). The else branch is gone — every theme now renders
// the flat Metro-style slider, still reading its own getTheme() colours.
void DysektLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float minSliderPos, float maxSliderPos,
                                          const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto& t = getTheme();
    auto bounds = juce::Rectangle<float> ((float)x, (float)y, (float)width, (float)height);

    if (style == juce::Slider::LinearVertical)
    {
        // Not currently used anywhere in the plugin, but keep a sane vertical
        // fallback to stock drawing rather than mis-rendering a horizontal bar.
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float trackH = 3.0f;   // thin flat track — was Metro-only, now universal
    auto track = bounds.withSizeKeepingCentre (bounds.getWidth(), trackH);
    g.setColour (t.gridLine);
    g.fillRect (track);

    const float fillLeft  = track.getX();
    const float fillRight = juce::jlimit (fillLeft, track.getRight(), sliderPos);
    if (fillRight > fillLeft)
    {
        g.setColour (t.accent);
        g.fillRect (juce::Rectangle<float> (fillLeft, track.getY(), fillRight - fillLeft, track.getHeight()));
    }

    const float thumbD = juce::jmin (bounds.getHeight(), 14.0f);
    auto thumb = juce::Rectangle<float> (thumbD, thumbD).withCentre ({ sliderPos, bounds.getCentreY() });
    g.setColour (t.accent);
    g.fillEllipse (thumb);
}

// ── Rotary slider ─────────────────────────────────────────────────────────────
// COLLAPSED: this used to branch metro (thin arc track + accent fill arc +
// indicator line) vs. every other theme (recessed gradient arc + glow fill
// arc + glassy beveled thumb dot). The else branch is gone — every theme now
// renders the flat Metro-style rotary, still reading its own getTheme() colours.
void DysektLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPosProportional, float rotaryStartAngle,
                                          float rotaryEndAngle, juce::Slider& slider)
{
    const auto& t = getTheme();
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const float lineW = juce::jmin (4.0f, radius * 0.22f);
    const float arcR  = radius - lineW * 0.5f;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (t.gridLine);
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (angle > rotaryStartAngle)
    {
        juce::Path fill;
        fill.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, angle, true);
        g.setColour (t.accent);
        g.strokePath (fill, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Simple indicator line from centre toward the pointer tip — flat, no glow
    juce::Point<float> tip (centre.x + std::sin (angle) * radius, centre.y - std::cos (angle) * radius);
    g.setColour (t.accent);
    g.drawLine (centre.x, centre.y, tip.x, tip.y, 2.0f);
}

// ── TextEditor ────────────────────────────────────────────────────────────────
// Flat fill + flat outline, matching drawButtonBackground's shape language.
void DysektLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    auto bg = te.findColour (juce::TextEditor::backgroundColourId);
    if (bg.isTransparent())
        bg = getTheme().darkBar.brighter (0.04f);

    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 0.0f);
}

void DysektLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
    if (te.isReadOnly())
        return;

    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
    const bool focused = te.hasKeyboardFocus (true);
    g.setColour (focused ? getTheme().accent
                          : getTheme().separator.withAlpha (0.60f));
    g.drawRoundedRectangle (bounds, 0.0f, focused ? 1.4f : 1.0f);
}

//==============================================================================
// AlertWindow overrides — make every SF-player popup readable
//==============================================================================
juce::Font DysektLookAndFeel::getAlertWindowTitleFont()
{
    return makeFont (16.0f * sMenuScale, true);
}

juce::Font DysektLookAndFeel::getAlertWindowMessageFont()
{
    return makeFont (14.0f * sMenuScale);
}

int DysektLookAndFeel::getAlertWindowButtonHeight()
{
    return juce::roundToInt (28.0f * sMenuScale);
}

void DysektLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                                      const juce::Rectangle<int>& textArea,
                                      juce::TextLayout& textLayout)
{
    const auto& t = getTheme();
    const float r = 0.0f;   // flattened — was a fixed 4.0f radius regardless of theme

    // Background
    g.setColour (t.darkBar);
    g.fillRoundedRectangle (alert.getLocalBounds().toFloat(), r);

    // Accent border
    g.setColour (t.accent.withAlpha (0.6f));
    g.drawRoundedRectangle (alert.getLocalBounds().toFloat().reduced (0.5f), r, 1.0f);

    // Title strip
    const auto titleArea = juce::Rectangle<int> (0, 0, alert.getWidth(), textArea.getY());
    g.setColour (t.accent.withAlpha (0.15f));
    g.fillRoundedRectangle (titleArea.toFloat(), r);

    g.setColour (t.foreground);
    g.setFont (getAlertWindowTitleFont());
    g.drawFittedText (alert.getName(),
                      titleArea.reduced (8, 4),
                      juce::Justification::centredLeft, 1);

    // Message body
    textLayout.draw (g, textArea.toFloat());
}

//==============================================================================
//  DocumentWindow title bar — themed to match DYSEKT-SF's dark palette
//==============================================================================

void DysektLookAndFeel::drawDocumentWindowTitleBar (
    juce::DocumentWindow& window, juce::Graphics& g,
    int w, int h, int titleSpaceX, int titleSpaceW,
    const juce::Image* /*icon*/, bool /*drawTitleTextOnLeft*/)
{
    const auto& t = getTheme();

    // Title bar background
    g.setColour (t.header);
    g.fillRect (0, 0, w, h);

    // Thin accent line along the bottom edge
    g.setColour (t.accent.withAlpha (0.35f));
    g.fillRect (0, h - 1, w, 1);

    // Title text
    g.setColour (t.foreground);
    g.setFont (makeFont (13.f, false));
    g.drawFittedText (window.getName(),
                      titleSpaceX + 4, 0, titleSpaceW - 8, h,
                      juce::Justification::centredLeft, 1);
}

//==============================================================================
//  Themed close / minimise / maximise buttons for DocumentWindow
//==============================================================================

namespace
{
    // A simple flat button that draws an X, –, or □ in DYSEKT-SF's colours.
    class DysektTitleBarButton : public juce::Button
    {
    public:
        enum class Kind { Close, Minimise, Maximise };

        DysektTitleBarButton (Kind k) : juce::Button ({}), kind (k) {}

        void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
        {
            const auto& t = getTheme();
            const auto  b = getLocalBounds().toFloat().reduced (2.f);
            const float r = 0.0f;   // flattened — was a fixed 3.f radius regardless of theme

            // Hover / press fill
            if (isDown)
                g.setColour (kind == Kind::Close ? juce::Colour (0xFFBB3333)
                                                 : t.buttonHover);
            else if (isHighlighted)
                g.setColour (kind == Kind::Close ? juce::Colour (0xFF883333)
                                                 : t.button);
            else
                g.setColour (juce::Colours::transparentBlack);

            g.fillRoundedRectangle (b, r);

            // Icon
            g.setColour (isHighlighted ? t.foreground : t.foreground.withAlpha (0.6f));
            const float cx = b.getCentreX(), cy = b.getCentreY();
            const float rad  = b.getHeight() * 0.22f;

            if (kind == Kind::Close)
            {
                g.drawLine (cx - rad, cy - rad, cx + rad, cy + rad, 1.5f);
                g.drawLine (cx + rad, cy - rad, cx - rad, cy + rad, 1.5f);
            }
            else if (kind == Kind::Minimise)
            {
                g.drawLine (cx - rad, cy, cx + rad, cy, 1.5f);
            }
            else  // Maximise
            {
                g.drawRect (juce::Rectangle<float> (cx - rad, cy - rad, rad * 2.f, rad * 2.f), 1.5f);
            }
        }

    private:
        Kind kind;
    };
}

juce::Button* DysektLookAndFeel::createDocumentWindowButton (int buttonType)
{
    using Kind = DysektTitleBarButton::Kind;
    if (buttonType == juce::DocumentWindow::closeButton)
        return new DysektTitleBarButton (Kind::Close);
    if (buttonType == juce::DocumentWindow::minimiseButton)
        return new DysektTitleBarButton (Kind::Minimise);
    return new DysektTitleBarButton (Kind::Maximise);
}
