#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

struct ThemeData;   // src/ui/ThemeData.h — only needed by buildTheme()

//==============================================================================
//  AvixLookAndFeel
//  "Modern cyber-industrial" skin layer (flat hardware plates, recessed
//  tracks, neon-green value strokes).  Skin only: it never touches component
//  bounds, resized(), grids or any other layout logic.
//
//  Honours the same per-button opt-in properties AvixLookAndFeel does
//  ("flatOutline", "flatFill", "transportFontSize") so existing call sites
//  keep working without edits.
//==============================================================================
class AvixLookAndFeel : public juce::LookAndFeel_V4
{
public:
    //==========================================================================
    //  Design tokens
    //==========================================================================
    struct Tokens
    {
        static inline const juce::Colour backgroundDark { juce::Colour::fromString ("ff0c0d10") };
        static inline const juce::Colour panelDark      { juce::Colour::fromString ("ff181b21") };
        static inline const juce::Colour accentGreen    { juce::Colour::fromString ("ff22ff66") };
        static inline const juce::Colour accentAmber    { juce::Colour::fromString ("ffffcc00") };
        static inline const juce::Colour textMain       { juce::Colour::fromString ("fff0f2f5") };
        static inline const juce::Colour textMuted      { juce::Colour::fromString ("ff6c7a89") };
        static inline const juce::Colour elementBorder  { juce::Colour::fromString ("ff2d3341") };

        static constexpr float cornerRadius      = 3.0f;   // buttons, fields, tracks
        static constexpr float cornerRadiusLarge = 4.0f;   // popups, alerts (max 5.0f)
    };

    enum class FontFace
    {
        jetBrainsMono,      // monospaced — default, closest to the DYSEKT look
        barlowCondensed     // narrow sans — use if mono text is too wide for a fixed-size cell
    };

    explicit AvixLookAndFeel (FontFace face = FontFace::jetBrainsMono);
    ~AvixLookAndFeel() override;

    /** Fills every ThemeData slot from the tokens above. Keeps name == "metro"
        and the existing slice palette, so the app's hand-painted components
        (which read getTheme()) pick up the new palette and keep taking their
        existing flat-rendering branches. Pass the result to setTheme(). */
    static ThemeData buildTheme();

    //==========================================================================
    //  Static utilities
    //==========================================================================
    static float getMenuScale(); // Exposed for internal plugin editor scaling

    //==========================================================================
    //  Typography
    //==========================================================================
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    int        getAlertWindowButtonHeight() override;

    // Static helpers for components that do not own a LookAndFeel instance.
    static juce::Font makeFont (float height, bool bold = false);
    static juce::Font makeMonoFont (float height, bool bold = false);

    // Instance typography preserves the selected FontFace for LookAndFeel callbacks.
    juce::Font makeFaceFont (float height, bool bold = false) const;

    //==========================================================================
    //  Buttons / toggles
    //==========================================================================
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool isEnabled, bool isHighlighted, bool isDown) override;

    //==========================================================================
    //  Sliders
    //==========================================================================
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    //==========================================================================
    //  ComboBox / popup menus / text fields
    //==========================================================================
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override;
    void drawPopupMenuUpDownArrow (juce::Graphics&, int width, int height, bool isScrollUpArrow) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline    (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    //==========================================================================
    //  Scrollbars / tooltips / alerts / document windows
    //==========================================================================
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;

    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea,
                       juce::TextLayout&) override;

    void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics&, int w, int h,
                                     int titleSpaceX, int titleSpaceW, const juce::Image* icon,
                                     bool drawTitleTextOnLeft) override;
    juce::Button* createDocumentWindowButton (int buttonType) override;

private:
    void registerColours();

    FontFace face;
    juce::Typeface::Ptr regularTypeface, boldTypeface;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AvixLookAndFeel)
};
