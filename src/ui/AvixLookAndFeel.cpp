cpp
#include "AvixLookAndFeel.h"
#include "ThemeData.h"
#include "ToolIcons.h"
#include "BinaryData.h"

using T = AvixLookAndFeel::Tokens;

//==============================================================================
namespace
{
    /** Flat matte plate with a microscopic (~3%) top-to-bottom fade. */
    void fillPlate (juce::Graphics& g, const juce::Path& p, juce::Rectangle<float> b, juce::Colour c)
    {
        g.setGradientFill (juce::ColourGradient (c.brighter (0.035f), b.getX(), b.getY(),
                                                 c.darker   (0.035f), b.getX(), b.getBottom(), false));
        g.fillPath (p);
    }

    /** Recessed bed: the fade runs the other way (darker at the top edge). */
    void fillRecess (juce::Graphics& g, const juce::Path& p, juce::Rectangle<float> b, juce::Colour c)
    {
        g.setGradientFill (juce::ColourGradient (c.darker   (0.035f), b.getX(), b.getY(),
                                                 c.brighter (0.035f), b.getX(), b.getBottom(), false));
        g.fillPath (p);
    }

    juce::Path roundedPath (juce::Rectangle<float> b, float r)
    {
        juce::Path p;
        p.addRoundedRectangle (b, r);
        return p;
    }

    juce::Colour dimmedAccent (float amount) { return T::panelDark.interpolatedWith (T::accentGreen, amount); }

    float buttonFontSize (int h) { return h < 16 ? 8.0f : h < 22 ? 10.0f : h < 28 ? 11.0f : 13.0f; }

    //==========================================================================
    class AvixTitleBarButton : public juce::Button
    {
    public:
        enum class Kind { close, minimise, maximise };
        explicit AvixTitleBarButton (Kind k) : juce::Button ({}), kind (k) {}

        void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
        {
            auto b = getLocalBounds().toFloat().reduced (2.0f);
            const bool close = (kind == Kind::close);

            if (isHighlighted || isDown)
            {
                g.setColour (close ? T::accentAmber.withAlpha (isDown ? 1.0f : 0.85f)
                                   : T::accentGreen.withAlpha (isDown ? 0.28f : 0.14f));
                g.fillRoundedRectangle (b, T::cornerRadius);
            }

            g.setColour ((isHighlighted && close) ? T::backgroundDark
                                                  : (isHighlighted ? T::textMain : T::textMuted));
            const float cx = b.getCentreX(), cy = b.getCentreY(), r = b.getHeight() * 0.22f;

            if (kind == Kind::close)
            {
                g.drawLine (cx - r, cy - r, cx + r, cy + r, 1.25f);
                g.drawLine (cx + r, cy - r, cx - r, cy + r, 1.25f);
            }
            else if (kind == Kind::minimise)
                g.drawLine (cx - r, cy, cx + r, cy, 1.25f);
            else
                g.drawRect (juce::Rectangle<float> (cx - r, cy - r, r * 2.0f, r * 2.0f), 1.25f);
        }

    private:
        Kind kind;
    };
}

//==============================================================================
//  Static Fallback for getMenuScale (Retained from project requirements)
//==============================================================================
float AvixLookAndFeel::getMenuScale()
{
    return 1.0f; // Default scale multiplier if fallback implementation is called
}

//==============================================================================
//  Construction / colour registration
//==============================================================================
AvixLookAndFeel::AvixLookAndFeel (FontFace f) : face (f)
{
    if (face == FontFace::jetBrainsMono)
    {
        regularTypeface = juce::Typeface::createSystemTypefaceFor (
            BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
        boldTypeface = juce::Typeface::createSystemTypefaceFor (
            BinaryData::JetBrainsMonoBold_ttf, BinaryData::JetBrainsMonoBold_ttfSize);
    }
    else
    {
        regularTypeface = juce::Typeface::createSystemTypefaceFor (
            BinaryData::BarlowCondensedRegular_ttf, BinaryData::BarlowCondensedRegular_ttfSize);
        boldTypeface = juce::Typeface::createSystemTypefaceFor (
            BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize);
    }

    registerColours();
}

AvixLookAndFeel::~AvixLookAndFeel()
{
    regularTypeface = nullptr;
    boldTypeface    = nullptr;
}

void AvixLookAndFeel::registerColours()
{
    using namespace juce;
    const auto clear = Colours::transparentBlack;
    const auto glow  = T::accentGreen.withAlpha (0.14f);   // selection / hover wash
    const auto sel   = T::accentGreen.withAlpha (0.28f);   // text selection

    // 1) Base scheme first — LookAndFeel_V4 fans this out to dozens of IDs
    setColourScheme (ColourScheme (T::backgroundDark,   // windowBackground
                                   T::panelDark,        // widgetBackground
                                   T::panelDark,        // menuBackground
                                   T::elementBorder,    // outline
                                   T::textMain,         // defaultText
                                   T::accentGreen,      // defaultFill
                                   T::backgroundDark,   // highlightedText
                                   T::accentGreen,      // highlightedFill
                                   T::textMain));       // menuText

    // 2) Windows & canvases
    setColour (ResizableWindow::backgroundColourId,  T::backgroundDark);
    setColour (DocumentWindow::textColourId,         T::textMain);
    setColour (AlertWindow::backgroundColourId,      T::panelDark);
    setColour (AlertWindow::textColourId,            T::textMain);
    setColour (AlertWindow::outlineColourId,         T::elementBorder);

    // 3) Buttons
    setColour (TextButton::buttonColourId,           T::panelDark);
    setColour (TextButton::buttonOnColourId,         dimmedAccent (0.22f));
    setColour (TextButton::textColourOffId,          T::textMain);
    setColour (TextButton::textColourOnId,           T::accentGreen);
    setColour (ToggleButton::textColourId,           T::textMain);
    setColour (ToggleButton::tickColourId,           T::accentGreen);
    setColour (ToggleButton::tickDisabledColourId,   T::textMuted);
    setColour (HyperlinkButton::textColourId,        T::accentGreen);

    // 4) Labels & text entry
    setColour (Label::textColourId,                  T::textMain);
    setColour (Label::backgroundColourId,            clear);
    setColour (Label::outlineColourId,               clear);
    setColour (Label::backgroundWhenEditingColourId, T::backgroundDark);
    setColour (Label::textWhenEditingColourId,       T::textMain);
    setColour (Label::outlineWhenEditingColourId,    T::accentGreen);

    setColour (TextEditor::backgroundColourId,       T::backgroundDark);
    setColour (TextEditor::textColourId,             T::textMain);
    setColour (TextEditor::highlightColourId,        sel);
    setColour (TextEditor::highlightedTextColourId,  T::textMain);
    setColour (TextEditor::outlineColourId,          T::elementBorder);
    setColour (TextEditor::focusedOutlineColourId,   T::accentGreen);
    setColour (TextEditor::shadowColourId,           clear);
    setColour (CaretComponent::caretColourId,        T::accentGreen);

    // 5) Sliders
    setColour (Slider::backgroundColourId,           T::backgroundDark);
    setColour (Slider::thumbColourId,                T::accentGreen);
    setColour (Slider::trackColourId,                T::accentGreen);
    setColour (Slider::rotarySliderFillColourId,     T::accentGreen);
    setColour (Slider::rotarySliderOutlineColourId,  T::elementBorder);
    setColour (Slider::textBoxTextColourId,          T::textMain);
    setColour (Slider::textBoxBackgroundColourId,    T::backgroundDark);
    setColour (Slider::textBoxHighlightColourId,     sel);
    setColour (Slider::textBoxOutlineColourId,       T::elementBorder);

    // 6) ComboBox & popup menus
    setColour (ComboBox::backgroundColourId,         T::panelDark);
    setColour (ComboBox::textColourId,               T::textMain);
    setColour (ComboBox::outlineColourId,            T::elementBorder);
    setColour (ComboBox::buttonColourId,             T::panelDark);
    setColour (ComboBox::arrowColourId,              T::textMuted);
    setColour (ComboBox::focusedOutlineColourId,     T::accentGreen);

    setColour (PopupMenu::backgroundColourId,            T::panelDark);
    setColour (PopupMenu::textColourId,                  T::textMain);
    setColour (PopupMenu::headerTextColourId,            T::textMuted);
    setColour (PopupMenu::highlightedBackgroundColourId, glow);
    setColour (PopupMenu::highlightedTextColourId,       T::accentGreen);

    // 7) Scrollbars, tooltips, lists, trees, tables, tabs, groups, progress
    setColour (ScrollBar::backgroundColourId,        T::backgroundDark);
    setColour (ScrollBar::trackColourId,             T::backgroundDark);
    setColour (ScrollBar::thumbColourId,             T::textMuted.withAlpha (0.45f));

    setColour (TooltipWindow::backgroundColourId,    T::panelDark);
    setColour (TooltipWindow::textColourId,          T::textMain);
    setColour (TooltipWindow::outlineColourId,       T::elementBorder);

    setColour (ListBox::backgroundColourId,          T::backgroundDark);
    setColour (ListBox::outlineColourId,             T::elementBorder);
    setColour (ListBox::textColourId,                T::textMain);

    setColour (TreeView::backgroundColourId,         T::backgroundDark);
    setColour (TreeView::linesColourId,              T::elementBorder);
    setColour (TreeView::dragAndDropIndicatorColourId, T::accentGreen);
    setColour (TreeView::selectedItemBackgroundColourId, glow);
    setColour (TreeView::oddItemsColourId,           clear);
    setColour (TreeView::evenItemsColourId,          clear);

Use code with caution.
setColour (TableHeaderComponent::textColourId,       T::textMain);
setColour (TableHeaderComponent::backgroundColourId, T::panelDark);
setColour (TableHeaderComponent::outlineColourId,    T::elementBorder);
setColour (TableHeaderComponent::highlightColourId,  glow);
setColour (TabbedButtonBar::tabOutlineColourId,  T::elementBorder);
setColour (TabbedButtonBar::tabTextColourId,     T::textMuted);
setColour (TabbedButtonBar::frontOutlineColourId, T::accentGreen);
setColour (TabbedButtonBar::frontTextColourId,   T::accentGreen);
setColour (TabbedComponent::backgroundColourId,  T::backgroundDark);
setColour (TabbedComponent::outlineColourId,     T::elementBorder);
setColour (GroupComponent::outlineColourId,      T::elementBorder);
setColour (GroupComponent::textColourId,         T::textMuted);
setColour (ProgressBar::backgroundColourId,      T::backgroundDark);
setColour (ProgressBar::foregroundColourId,      T::accentGreen);
// 8) Property panels, file browsing, toolbars, misc dialogs
setColour (PropertyComponent::backgroundColourId, T::panelDark);
setColour (PropertyComponent::labelTextColourId,  T::textMain);
setColour (PropertyComponent::outlineColourId,    T::elementBorder);
setColour (TextPropertyComponent::backgroundColourId, T::backgroundDark);
setColour (TextPropertyComponent::textColourId,       T::textMain);
setColour (TextPropertyComponent::outlineColourId,    T::elementBorder);
setColour (BooleanPropertyComponent::backgroundColourId, T::backgroundDark);
setColour (BooleanPropertyComponent::outlineColourId,    T::elementBorder);
setColour (DirectoryContentsDisplayComponent::highlightColourId,      glow);
setColour (DirectoryContentsDisplayComponent::textColourId,           T::textMain);
setColour (DirectoryContentsDisplayComponent::highlightedTextColourId, T::accentGreen);
setColour (FileBrowserComponent::currentPathBoxBackgroundColourId, T::backgroundDark);
setColour (FileBrowserComponent::currentPathBoxTextColourId,       T::textMain);
setColour (FileBrowserComponent::currentPathBoxArrowColourId,      T::textMuted);
setColour (FileBrowserComponent::filenameBoxBackgroundColourId,    T::backgroundDark);
setColour (FileBrowserComponent::filenameBoxTextColourId,          T::textMain);
setColour (FileChooserDialogBox::titleTextColourId,                T::textMain);
setColour (Toolbar::backgroundColourId,                    T::panelDark);
setColour (Toolbar::separatorColourId,                     T::elementBorder);
setColour (Toolbar::labelTextColourId,                     T::textMuted);
setColour (Toolbar::buttonMouseOverBackgroundColourId,     glow);
setColour (Toolbar::buttonMouseDownBackgroundBackgroundColourId, sel);
setColour (ColourSelector::backgroundColourId,   T::backgroundDark);
setColour (ColourSelector::labelTextColourId,    T::textMain);
setColour (KeyMappingEditorComponent::backgroundColourId, T::backgroundDark);
setColour (KeyMappingEditorComponent::textColourId,       T::textMain);
}
//==============================================================================
//  Theme factory for the app's hand-painted components
//==============================================================================
ThemeData AvixLookAndFeel::buildTheme()
{
ThemeData t = ThemeData::metroTheme();
t.background       = T::backgroundDark;
t.waveformBg       = T::backgroundDark;
t.darkBar          = T::panelDark;
t.foreground       = T::textMain;
t.header           = T::backgroundDark;
t.waveform         = T::accentGreen;
t.selectionOverlay = T::accentGreen.withAlpha (0.18f);
t.lockActive       = T::accentAmber;
t.lockInactive     = T::textMuted;
t.gridLine         = T::elementBorder.darker (0.35f);
t.accent           = T::accentGreen;
t.button           = T::panelDark;
t.buttonHover      = T::panelDark.interpolatedWith (T::accentGreen, 0.10f);
t.separator        = T::elementBorder;
return t;
}
//==============================================================================
//  Typography
//==============================================================================
namespace
{
    juce::Typeface::Ptr sharedMonoTypeface (bool bold)
    {
        static auto regular = juce::Typeface::createSystemTypefaceFor (
            BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
        static auto boldFace = juce::Typeface::createSystemTypefaceFor (
            BinaryData::JetBrainsMonoBold_ttf, BinaryData::JetBrainsMonoBold_ttfSize);
        return bold ? boldFace : regular;
    }
}

juce::Font AvixLookAndFeel::makeFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions().withHeight (height)
                                           .withTypeface (sharedMonoTypeface (bold)));
}

juce::Font AvixLookAndFeel::makeMonoFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions().withHeight (height)
                                           .withTypeface (sharedMonoTypeface (bold)));
}

juce::Font AvixLookAndFeel::makeFaceFont (float height, bool bold) const
{
auto tf = bold ? boldTypeface : regularTypeface;
auto opts = juce::FontOptions().withHeight (height);
if (tf != nullptr)
opts = opts.withTypeface (tf);
return juce::Font (opts);
}
juce::Typeface::Ptr AvixLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
const auto style = f.getTypefaceStyle().toLowerCase();
const bool bold = f.isBold() || style.contains ("bold") || style.contains ("semi");
auto tf = bold ? boldTypeface : regularTypeface;
return tf != nullptr ? tf : juce::LookAndFeel_V4::getTypefaceForFont (f);
}
juce::Font AvixLookAndFeel::getLabelFont (juce::Label& label)
{
const auto f = label.getFont();
return makeFaceFont (f.getHeight(), f.isBold());
}
juce::Font AvixLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
return makeFaceFont (juce::jmin (16.0f, (float) buttonHeight * 0.6f));
}
juce::Font AvixLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
const auto requested = (double) box.getProperties().getWithDefault ("transportFontSize", 0.0);
return makeFaceFont (requested > 0.0 ? (float) requested : 14.0f * menuScale());
}
juce::Font AvixLookAndFeel::getPopupMenuFont()          { return makeFaceFont (14.0f * menuScale()); }
juce::Font AvixLookAndFeel::getAlertWindowTitleFont()   { return makeFaceFont (16.0f * menuScale(), true); }
juce::Font AvixLookAndFeel::getAlertWindowMessageFont() { return makeFaceFont (14.0f * menuScale()); }
int        AvixLookAndFeel::getAlertWindowButtonHeight(){ return juce::roundToInt (28.0f * menuScale()); }
//==============================================================================
//  Buttons
//==============================================================================
void AvixLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
const juce::Colour& /backgroundColour/,
bool isHighlighted, bool isDown)
{
if (button.findColour (juce::TextButton::buttonColourId).isTransparent())
return;
const auto bounds   = button.getLocalBounds().toFloat().reduced (0.5f);
const bool toggled  = button.getToggleState();
const bool enabled  = button.isEnabled();
const float r       = T::cornerRadius;
const auto& props   = button.getProperties();
if ((bool) props.getWithDefault ("flatOutline", false))
{
g.setColour ((toggled ? T::accentGreen
: isHighlighted ? T::accentGreen.withAlpha (0.75f) : T::elementBorder)
.withMultipliedAlpha (enabled ? 1.0f : 0.45f));
g.drawRoundedRectangle (bounds, r, 1.0f);
return;
}
if ((bool) props.getWithDefault ("flatFill", false))
{
auto role = toggled ? button.findColour (juce::TextButton::buttonOnColourId)
: button.findColour (juce::TextButton::buttonColourId);
if (! enabled) role = role.withMultipliedAlpha (0.45f);
const auto p = roundedPath (bounds, r);
fillPlate (g, p, bounds, role);
g.setColour (isHighlighted || isDown ? T::accentGreen.withAlpha (0.7f) : T::elementBorder);
g.strokePath (p, juce::PathStrokeType (1.0f));
return;
}
auto fill   = T::panelDark;
auto border = T::elementBorder;
if (isDown)            { fill = dimmedAccent (0.24f); border = T::accentGreen; }
else if (toggled)      { fill = dimmedAccent (0.16f); border = T::accentGreen.withAlpha (0.90f); }
else if (isHighlighted){ fill = dimmedAccent (0.07f); border = T::accentGreen.withAlpha (0.65f); }
if (! enabled)
{
fill   = fill.withMultipliedAlpha (0.6f);
border = border.withMultipliedAlpha (0.6f);
}
using B = juce::Button;
const int f = button.getConnectedEdgeFlags();
juce::Path p;
p.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), r, r,
(f & (B::ConnectedOnLeft  | B::ConnectedOnTop))    == 0,
(f & (B::ConnectedOnRight | B::ConnectedOnTop))    == 0,
(f & (B::ConnectedOnLeft  | B::ConnectedOnBottom)) == 0,
(f & (B::ConnectedOnRight | B::ConnectedOnBottom)) == 0);
fillPlate (g, p, bounds, fill);
g.setColour (border);
g.strokePath (p, juce::PathStrokeType (1.0f));
}
void AvixLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
bool /isHighlighted/, bool /isDown/)
{
const bool toggled = button.getToggleState();
const auto& props  = button.getProperties();
juce::Colour textCol;
if ((bool) props.getWithDefault ("flatOutline", false))
textCol = T::accentGreen;
else
{
textCol = button.findColour (toggled ? juce::TextButton::textColourOnId
: juce::TextButton::textColourOffId);
if (textCol.isTransparent())
textCol = toggled ? T::accentGreen : T::textMain;
}
if (! button.isEnabled())
textCol = textCol.withMultipliedAlpha (0.45f);
g.setColour (textCol);
if ((bool) props.getWithDefault ("flatFill", false))
{
const auto txt = button.getButtonText();
ToolIcons::Kind kind {};
bool isTool = true;
if      (txt == "SELECT") kind = ToolIcons::Kind::Select;
else if (txt == "DRAW")   kind = ToolIcons::Kind::Draw;
else if (txt == "ERASE")  kind = ToolIcons::Kind::Erase;
else if (txt == "SPLIT")  kind = ToolIcons::Kind::Split;
else if (txt == "GLUE")   kind = ToolIcons::Kind::Glue;
else                      isTool = false;
if (isTool)
{
ToolIcons::draw (g, kind, button.getLocalBounds().toFloat().reduced (7.0f, 4.0f), textCol);
return;
}
}
const int h = button.getHeight();
const auto txt = button.getButtonText();
const bool isSymbol = (txt.length() <= 2 && txt[0] > 127);
float fontSize = isSymbol ? (h < 22 ? 14.0f : 17.0f) : buttonFontSize (h);
const auto requested = (double) props.getWithDefault ("transportFontSize", 0.0);
if (requested > 0.0)
fontSize = (float) requested;
g.setFont (makeFaceFont (fontSize));
g.drawFittedText (txt, button.getLocalBounds().reduced (2, 0),
juce::Justification::centred, 1, 0.6f);
}
void AvixLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& c,
float x, float y, float w, float h,
bool ticked, bool isEnabled, bool isHighlighted, bool isDown)
{
auto box = juce::Rectangle (x, y, w, h).reduced (0.5f);
const float a = isEnabled ? 1.0f : 0.45f;
const auto p = roundedPath (box, T::cornerRadius);
fillRecess (g, p, box, T::backgroundDark.withMultipliedAlpha (a));
g.setColour ((ticked || isHighlighted || isDown ? T::accentGreen.withAlpha (ticked ? 0.9f : 0.6f)
: T::elementBorder).withMultipliedAlpha (a));
g.strokePath (p, juce::PathStrokeType (1.0f));
if (ticked)
{
g.setColour (c.findColour (juce::ToggleButton::tickColourId).withMultipliedAlpha (a));
g.fillRoundedRectangle (box.reduced (box.getWidth() * 0.26f), 1.5f);
}
}
void AvixLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
bool isHighlighted, bool isDown)
{
const float fontSize  = juce::jmin (15.0f, (float) button.getHeight() * 0.75f);
const float tickWidth = fontSize * 1.1f;
drawTickBox (g, button, 4.0f, ((float) button.getHeight() - tickWidth) * 0.5f, tickWidth, tickWidth,
button.getToggleState(), button.isEnabled(), isHighlighted, isDown);
g.setColour (button.findColour (juce::ToggleButton::textColourId)
.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
g.setFont (makeFaceFont (fontSize));
g.drawFittedText (button.getButtonText(),
button.getLocalBounds().withTrimmedLeft (juce::roundToInt (tickWidth) + 10).withTrimmedRight (2),
juce::Justification::centredLeft, 1, 0.6f);
}
//==============================================================================
//  Sliders
//==============================================================================
void AvixLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
float sliderPos, float startAngle, float endAngle,
juce::Slider& slider)
{
auto area = juce::Rectangle ((float) x, (float) y, (float) width, (float) height).reduced (2.0f);
const float size = juce::jmin (area.getWidth(), area.getHeight());
if (size < 8.0f)
return;
const auto  c = area.getCentre();
const float R = size * 0.5f;
const float a = slider.isEnabled() ? 1.0f : 0.4f;
const float ringW = juce::jlimit (1.5f, 3.0f, R * 0.12f);
const float arcR  = R - 1.0f - ringW * 0.5f - 1.0f;
const float bodyR = juce::jmax (2.0f, arcR - ringW * 0.5f - juce::jmax (2.0f, R * 0.10f));
{
auto bed = juce::Rectangle (R * 2.0f, R * 2.0f).withCentre (c).reduced (0.5f);
juce::Path p; p.addEllipse (bed);
fillRecess (g, p, bed, T::backgroundDark.withMultipliedAlpha (a));
g.setColour (T::elementBorder.withMultipliedAlpha (a));
g.drawEllipse (bed, 1.0f);
}
const float angle = startAngle + sliderPos * (endAngle - startAngle);
const juce::PathStrokeType stroke (ringW, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
{
juce::Path track;
track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
g.setColour (T::elementBorder.brighter (0.15f).withMultipliedAlpha (a));
g.strokePath (track, stroke);
}
float fromAngle = startAngle;
if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
fromAngle = startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle);
const float a0 = juce::jmin (fromAngle, angle), a1 = juce::jmax (fromAngle, angle);
if (a1 - a0 > 0.005f)
{
juce::Path arc;
arc.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, a0, a1, true);
if (slider.isMouseOverOrDragging() && slider.isEnabled())
{
g.setColour (T::accentGreen.withAlpha (0.16f));
g.strokePath (arc, juce::PathStrokeType (ringW + 3.0f, juce::PathStrokeType::curved,
juce::PathStrokeType::butt));
}
g.setColour (T::accentGreen.withMultipliedAlpha (a));
g.strokePath (arc, stroke);
}
{
auto cap = juce::Rectangle (bodyR * 2.0f, bodyR * 2.0f).withCentre (c);
juce::Path p; p.addEllipse (cap);
fillPlate (g, p, cap, T::panelDark.withMultipliedAlpha (a));
g.setColour (T::elementBorder.withMultipliedAlpha (a));
g.drawEllipse (cap.reduced (0.5f), 1.0f);
}
const float s = std::sin (angle), co = std::cos (angle);
const juce::Point p0 (c.x + s * bodyR * 0.30f, c.y - co * bodyR * 0.30f);
const juce::Point p1 (c.x + s * (bodyR - 1.5f), c.y - co * (bodyR - 1.5f));
g.setColour (T::accentGreen.withMultipliedAlpha (a));
g.drawLine (juce::Line (p0, p1), 1.25f);
if (slider.isEnabled())
{
const float dr = juce::jmax (1.5f, bodyR * 0.10f);
g.setColour (T::accentGreen.withAlpha (0.18f));
g.fillEllipse (juce::Rectangle (dr * 4.0f, dr * 4.0f).withCentre (p1));
g.setColour (T::accentGreen.withAlpha (0.40f));
g.fillEllipse (juce::Rectangle (dr * 2.6f, dr * 2.6f).withCentre (p1));
g.setColour (T::textMain);
g.fillEllipse (juce::Rectangle (dr * 1.2f, dr * 1.2f).withCentre (p1));
}
}
void AvixLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
float sliderPos, float minSliderPos, float maxSliderPos,
juce::Slider::SliderStyle style, juce::Slider& slider)
{
if (slider.isBar() || slider.isTwoValue() || slider.isThreeValue())
{
juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
minSliderPos, maxSliderPos, style, slider);
return;
}
const bool horiz = slider.isHorizontal();
const float a = slider.isEnabled() ? 1.0f : 0.45f;
const auto bounds = juce::Rectangle ((float) x, (float) y, (float) width, (float) height);
constexpr float channelT = 6.0f;
const auto channel = horiz ? bounds.withSizeKeepingCentre (bounds.getWidth(), channelT)
: bounds.withSizeKeepingCentre (channelT, bounds.getHeight());
{
const auto p = roundedPath (channel.reduced (0.5f), T::cornerRadius);
fillRecess (g, p, channel, T::backgroundDark.withMultipliedAlpha (a));
g.setColour (T::elementBorder.withMultipliedAlpha (a));
g.strokePath (p, juce::PathStrokeType (1.0f));
}
float fromPos = minSliderPos;
const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
if (bipolar)
fromPos = minSliderPos + (float) slider.valueToProportionOfLength (0.0) * (maxSliderPos - minSliderPos);
else if (! horiz)
fromPos = channel.getBottom();
else
fromPos = channel.getX();
const auto inner = channel.reduced (1.0f);
juce::Rectangle fill;
if (horiz)
{
const float l = juce::jlimit (inner.getX(), inner.getRight(), juce::jmin (fromPos, sliderPos));
const float r = juce::jlimit (inner.getX(), inner.getRight(), juce::jmax (fromPos, sliderPos));
fill = { l, inner.getY(), r - l, inner.getHeight() };
}
else
{
const float t = juce::jlimit (inner.getY(), inner.getBottom(), juce::jmin (fromPos, sliderPos));
const float b = juce::jlimit (inner.getY(), inner.getBottom(), juce::jmax (fromPos, sliderPos));
fill = { inner.getX(), t, inner.getWidth(), b - t };
}
if (! fill.isEmpty())
{
g.setColour (T::accentGreen.withMultipliedAlpha (a));
g.fillRoundedRectangle (fill, 1.5f);
}
if (bipolar)
{
g.setColour (T::textMuted.withMultipliedAlpha (a));
if (horiz) g.fillRect (fromPos - 0.5f, channel.getY() - 2.0f, 1.0f, channelT + 4.0f);
else       g.fillRect (channel.getX() - 2.0f, fromPos - 0.5f, channelT + 4.0f, 1.0f);
}
const float thumbLong  = 8.0f;
const float thumbShort = horiz ? juce::jmin ((float) height, 18.0f) : juce::jmin ((float) width, 18.0f);
const auto thumb = horiz ? juce::Rectangle (thumbLong, thumbShort).withCentre ({ sliderPos, bounds.getCentreY() })
: juce::Rectangle (thumbShort, thumbLong).withCentre ({ bounds.getCentreX(), sliderPos });
const auto thumbCol = T::panelDark.interpolatedWith (T::textMain, 0.14f).withMultipliedAlpha (a);
g.setColour (thumbCol);
g.fillRoundedRectangle (thumb, 1.5f);
g.setColour (T::elementBorder.brighter (0.5f).withMultipliedAlpha (a));
g.drawRoundedRectangle (thumb.reduced (0.5f), 1.5f, 1.0f);
g.setColour (T::accentGreen.withMultipliedAlpha (a));
if (horiz) g.fillRect (thumb.getCentreX() - 1.0f, thumb.getY() + 3.0f, 2.0f, thumb.getHeight() - 6.0f);
else       g.fillRect (thumb.getX() + 3.0f, thumb.getCentreY() - 1.0f, thumb.getWidth() - 6.0f, 2.0f);
}
//==============================================================================
//  ComboBox
//==============================================================================
void AvixLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
int buttonX, int /buttonY/, int /buttonW/, int /buttonH/,
juce::ComboBox& box)
{
const auto bounds = juce::Rectangle (0, 0, (float) width, (float) height).reduced (0.5f);
const float a = box.isEnabled() ? 1.0f : 0.5f;
const bool active = isButtonDown || box.isPopupActive() || box.hasKeyboardFocus (false);
const bool hover  = box.isMouseOver (true);
auto fill = T::panelDark;
if (active)     fill = dimmedAccent (0.10f);
else if (hover) fill = dimmedAccent (0.05f);
const auto p = roundedPath (bounds, T::cornerRadius);
fillPlate (g, p, bounds, fill.withMultipliedAlpha (a));
g.setColour ((active ? T::accentGreen : hover ? T::accentGreen.withAlpha (0.55f) : T::elementBorder)
.withMultipliedAlpha (a));
g.strokePath (p, juce::PathStrokeType (1.0f));
if (buttonX > 0 && buttonX < width)
{
g.setColour (T::elementBorder.withMultipliedAlpha (a));
g.fillRect (buttonX, 4, 1, juce::jmax (0, height - 8));
}
const float cx = (float) (buttonX + (width - buttonX) / 2);
const float cy = (float) height * 0.5f;
g.setColour ((active || hover ? T::accentGreen : T::textMuted).withMultipliedAlpha (a));
juce::Path chevron;
chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
chevron.lineTo (cx, cy + 2.0f);
chevron.lineTo (cx + 4.0f, cy - 2.0f);
g.strokePath (chevron, juce::PathStrokeType (1.5f));
}
void AvixLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
label.setBounds (1, 1, box.getWidth() - 28, box.getHeight() - 2);
label.setFont (getComboBoxFont (box));
label.setColour (juce::Label::textColourId, box.findColour (juce::ComboBox::textColourId));
label.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}
//==============================================================================
//  Popup menus
//==============================================================================
void AvixLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
const auto b = juce::Rectangle (0, 0, (float) width, (float) height);
g.fillAll (T::backgroundDark);
const auto p = roundedPath (b.reduced (0.5f), T::cornerRadiusLarge);
fillPlate (g, p, b, T::panelDark);
g.setColour (T::elementBorder);
g.strokePath (p, juce::PathStrokeType (1.0f));
}
void AvixLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle& area,
bool isSeparator, bool isActive, bool isHighlighted,
bool isTicked, bool hasSubMenu,
const juce::String& text, const juce::String& shortcutKeyText,
const juce::Drawable* icon, const juce::Colour* textColour)
{
const float ms = 1.0f; // Simplified using local default multiplier
if (isSeparator)
{
g.setColour (T::elementBorder);
g.fillRect (area.reduced (6, 0).withHeight (1).withY (area.getCentreY()));
return;
}
if (isHighlighted && isActive)
{
g.setColour (T::accentGreen.withAlpha (0.12f));
g.fillRoundedRectangle (area.reduced (3, 1).toFloat(), 2.0f);
g.setColour (T::accentGreen);
g.fillRect (juce::Rectangle ((float) area.getX() + 3.0f, (float) area.getY() + 3.0f,
2.0f, (float) area.getHeight() - 6.0f));
}
const int tickZoneW = (int) (16 * ms);
if (isTicked)
{
const int d = (int) (4 * ms);
g.setColour (T::accentGreen);
g.fillRect (area.getX() + (tickZoneW - d) / 2, area.getCentreY() - d / 2, d, d);
}
int contentLeft = area.getX() + tickZoneW;
if (icon != nullptr)
{
const int zone = (int) (22 * ms), sz = (int) (16 * ms);
auto ib = juce::Rectangle (contentLeft, area.getY(), zone, area.getHeight()).withSizeKeepingCentre (sz, sz);
icon->drawWithin (g, ib.toFloat(), juce::RectanglePlacement::centred, 1.0f);
contentLeft += zone;
}
juce::Colour col = ! isActive            ? T::textMuted.withAlpha (0.6f)
: isTicked              ? T::accentGreen
: (textColour != nullptr && ! isHighlighted) ? *textColour
: T::textMain;
g.setColour (col);
g.setFont (getPopupMenuFont());
const int rightPad = (int) (hasSubMenu ? 16 * ms : 6 * ms);
auto textArea = area.withLeft (contentLeft).withRight (area.getRight() - rightPad);
g.drawFittedText (text, textArea, juce::Justification::centredLeft, 1, 0.7f);
if (shortcutKeyText.isNotEmpty())
{
g.setColour (T::textMuted);
g.setFont (makeFaceFont (12.0f * ms));
g.drawText (shortcutKeyText, textArea, juce::Justification::centredRight);
}
if (hasSubMenu)
{
const float cx = (float) area.getRight() - 8.0f * ms, cy = (float) area.getCentreY(), s = 3.5f * ms;
juce::Path arrow;
arrow.startNewSubPath (cx - s * 0.5f, cy - s);
arrow.lineTo (cx + s * 0.5f, cy);
arrow.lineTo (cx - s * 0.5f, cy + s);
g.setColour (col);
g.strokePath (arrow, juce::PathStrokeType (1.25f));
}
}
void AvixLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle& area,
const juce::String& sectionName)
{
const float ms = 1.0f;
g.setFont (makeFaceFont (12.0f * ms, true));
g.setColour (T::textMuted);
g.drawFittedText (sectionName.toUpperCase(),
area.getX() + (int) (12 * ms), area.getY(),
area.getWidth() - (int) (16 * ms), (int) ((float) area.getHeight() * 0.8f),
juce::Justification::bottomLeft, 1, 0.7f);
}
void AvixLookAndFeel::drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height, bool isScrollUpArrow)
{
const float cx = (float) width * 0.5f, cy = (float) height * 0.5f, s = 4.0f;
juce::Path p;
if (isScrollUpArrow) p.addTriangle (cx - s, cy + s * 0.5f, cx + s, cy + s * 0.5f, cx, cy - s * 0.5f);
else                 p.addTriangle (cx - s, cy - s * 0.5f, cx + s, cy - s * 0.5f, cx, cy + s * 0.5f);
g.setColour (T::textMuted);
g.fillPath (p);
}
//==============================================================================
//  Text editors
//==============================================================================
void AvixLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
auto bg = te.findColour (juce::TextEditor::backgroundColourId);
if (bg.isTransparent())
bg = T::backgroundDark;
const auto b = juce::Rectangle (0, 0, (float) width, (float) height).reduced (0.5f);
const auto p = roundedPath (b, T::cornerRadius);
fillRecess (g, p, b, bg);
}
void AvixLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& te)
{
if (te.isReadOnly())
return;
const auto b = juce::Rectangle (0, 0, (float) width, (float) height).reduced (0.5f);
g.setColour (te.hasKeyboardFocus (true) ? T::accentGreen : T::elementBorder);
g.drawRoundedRectangle (b, T::cornerRadius, 1.0f);
}
//==============================================================================
//  Scrollbars / tooltips / alerts / document windows
//==============================================================================
void AvixLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
bool isVertical, int thumbStart, int thumbSize,
bool isMouseOver, bool isMouseDown)
{
g.setColour (T::backgroundDark);
g.fillRect (x, y, width, height);
g.setColour (T::elementBorder);
g.drawRect (x, y, width, height, 1);
if (thumbSize <= 0)
return;
auto thumb = isVertical ? juce::Rectangle (x + 1, y + thumbStart, width - 2, thumbSize)
: juce::Rectangle (x + thumbStart, y + 1, thumbSize, height - 2);
g.setColour (isMouseDown ? T::accentGreen
: isMouseOver ? T::accentGreen.withAlpha (0.70f)
: T::textMuted.withAlpha (0.45f));
g.fillRoundedRectangle (thumb.toFloat().reduced (1.0f), 2.0f);
}
void AvixLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
const auto b = juce::Rectangle (0, 0, (float) width, (float) height).reduced (0.5f);
const auto p = roundedPath (b, T::cornerRadius);
fillPlate (g, p, b, T::panelDark);
g.setColour (T::elementBorder);
g.strokePath (p, juce::PathStrokeType (1.0f));
g.setColour (T::textMain);
g.setFont (makeFaceFont (13.0f));
g.drawFittedText (text, 6, 0, width - 12, height, juce::Justification::centredLeft, 1, 0.7f);
}
juce::Rectangle AvixLookAndFeel::getTooltipBounds (const juce::String& text, juce::Point screenPos,
juce::Rectangle parentArea)
{
const int w = (int) juce::GlyphArrangement::getStringWidth (makeFaceFont (14.0f), text) + 14;
const int h = 24;
int x = screenPos.x, y = screenPos.y + 18;
if (x + w > parentArea.getRight())  x = parentArea.getRight() - w;
if (y + h > parentArea.getBottom()) y = screenPos.y - h - 4;
return { x, y, w, h };
}
void AvixLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
const juce::Rectangle& textArea, juce::TextLayout& textLayout)
{
const auto full = alert.getLocalBounds().toFloat();
const auto p = roundedPath (full.reduced (0.5f), T::cornerRadiusLarge);
fillPlate (g, p, full, T::panelDark);
const auto titleArea = juce::Rectangle (0, 0, alert.getWidth(), textArea.getY());
g.saveState();
g.reduceClipRegion (titleArea);
g.setColour (T::accentGreen.withAlpha (0.08f));
g.fillPath (p);
g.restoreState();
g.setColour (T::elementBorder);
g.fillRect (0, titleArea.getBottom() - 1, alert.getWidth(), 1);
g.setColour (T::elementBorder);
g.strokePath (p, juce::PathStrokeType (1.0f));
g.setColour (T::textMain);
g.setFont (getAlertWindowTitleFont());
g.drawFittedText (alert.getName(), titleArea.reduced (10, 4), juce::Justification::centredLeft, 1, 0.7f);
g.setColour (alert.findColour (juce::AlertWindow::textColourId));
textLayout.draw (g, textArea.toFloat());
}
void AvixLookAndFeel::drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g,
int w, int h, int titleSpaceX, int titleSpaceW,
const juce::Image*, bool)
{
g.setColour (T::backgroundDark);
g.fillRect (0, 0, w, h);
g.setColour (T::elementBorder);
g.fillRect (0, h - 1, w, 1);
g.setColour (T::accentGreen.withAlpha (0.55f));
g.fillRect (0, h - 1, juce::jmin (48, w), 1);
g.setColour (T::textMain);
g.setFont (makeFaceFont (13.0f));
g.drawFittedText (window.getName(), titleSpaceX + 4, 0, titleSpaceW - 8, h,
juce::Justification::centredLeft, 1, 0.7f);
}
juce::Button* AvixLookAndFeel::createDocumentWindowButton (int buttonType)
{
using K = AvixTitleBarButton::Kind;
if (buttonType == juce::DocumentWindow::closeButton)    return new AvixTitleBarButton (K::close);
if (buttonType == juce::DocumentWindow::minimiseButton) return new AvixTitleBarButton (K::minimise);
return new AvixTitleBarButton (K::maximise);
}