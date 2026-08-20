#pragma once
#include "UIHelpers.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

// Themed, single-button replacement for juce::AlertWindow::showMessageBoxAsync /
// juce::AlertWindow::showAsync. The stock AlertWindow only picks up the plugin's
// colours (see DysektLookAndFeel::drawAlertBox); its box shape, title strip, and
// button row are still the generic JUCE dialog layout. MessageOverlay reuses the
// exact chrome ConfirmOverlay/RenameOverlay already use (UIHelpers::drawPopupBackdrop
// / drawPopupBox / stylePrimaryPopupButton) so a plain info/warning popup looks
// like it belongs to the same dialog family as every other overlay in the plugin.
class MessageOverlay : public juce::Component
{
public:
    enum class Kind { Info, Warning };

    std::function<void()> onDismiss;

    MessageOverlay (const juce::String& title,
                    const juce::String& message,
                    Kind kind = Kind::Info,
                    const juce::String& buttonText = "OK")
        : titleText (title), messageText (message), messageKind (kind)
    {
        okBtn.setButtonText (buttonText);
        UIHelpers::stylePrimaryPopupButton (okBtn, getTheme());
        okBtn.onClick = [this] { fireDismiss(); };
        addAndMakeVisible (okBtn);

        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);
    }

    void visibilityChanged() override
    {
        if (isVisible())
            grabKeyboardFocus();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();

        UIHelpers::drawPopupBackdrop (g, getLocalBounds());

        const auto box = dialogBox();
        UIHelpers::drawPopupBox (g, box, T);

        // Warning titles pick up a warm amber instead of the theme accent so a
        // failed-write message still reads as "something went wrong" even in
        // themes where the accent colour itself isn't red/orange (e.g. Ice Blue).
        const auto titleColour = messageKind == Kind::Warning
                                ? juce::Colour (0xFFE0A030)
                                : T.accent;

        const int padX = 18;
        g.setFont (DysektLookAndFeel::makeFont (15.0f, true));
        g.setColour (titleColour);
        g.drawText (titleText,
                    box.getX() + padX, box.getY() + 14,
                    box.getWidth() - padX * 2, 20,
                    juce::Justification::centredLeft, false);

        // Fixed, always-legible size — no more g.drawFittedText() shrinking
        // this down to fit a hard-capped box; dialogBox() below grows to fit
        // this text instead. See UIHelpers::measureWrappedTextHeight/
        // drawWrappedText's comment for why.
        UIHelpers::drawWrappedText (g, messageText, bodyFont(), T.foreground.withAlpha (0.85f),
                    juce::Rectangle<float> ((float) (box.getX() + padX), (float) (box.getY() + 42),
                                             (float) (box.getWidth() - padX * 2),
                                             (float) (box.getHeight() - 42 - 46)));
    }

    void resized() override
    {
        const auto box = dialogBox();
        const int btnW = 96;
        const int btnH = 28;
        okBtn.setBounds (box.getCentreX() - btnW / 2, box.getBottom() - btnH - 14, btnW, btnH);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! dialogBox().contains (e.getPosition()))
            fireDismiss();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::escapeKey)
        {
            fireDismiss();
            return true;
        }
        return false;
    }

private:
    juce::String titleText, messageText;
    Kind messageKind;
    juce::TextButton okBtn;

    // okBtn.onClick/mouseDown/keyPressed all fire from inside our own or a
    // child component's click/key-handling code, and every caller's
    // onDismiss handler destroys `this` (messageOverlay.reset()) — see
    // ConfirmOverlay.h's constructor comment for the full explanation of why
    // that can't happen synchronously from in here. Deferring via callAsync
    // runs onDismiss() after that call stack has fully unwound, so it's
    // safe for the receiver to destroy us then.
    void fireDismiss()
    {
        juce::Component::SafePointer<MessageOverlay> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr && safeThis->onDismiss)
                safeThis->onDismiss();
        });
    }

    static juce::Font bodyFont() { return DysektLookAndFeel::makeFont (12.5f); }

    juce::Rectangle<int> dialogBox() const
    {
        const int padX = 18;
        const int w = juce::jmin (420, getWidth() - 40);
        const int textWidth = juce::jmax (10, w - padX * 2);

        // 42 = top offset of the text block (matches paint()); 46 = space
        // reserved below it for the button row + bottom margin (matches
        // resized()'s okBtn placement, 14px above the box bottom).
        const int textH   = UIHelpers::measureWrappedTextHeight (messageText, bodyFont(), textWidth);
        const int contentH = 42 + textH + 46;

        // Never smaller than the original fixed size (keeps short messages
        // looking the same as before), never taller than the window allows
        // minus a margin — drawWrappedText() clips cleanly if a message is
        // so long it still doesn't fit even at that cap.
        const int maxH = juce::jmax (156, getHeight() - 40);
        const int h = juce::jlimit (156, maxH, contentH);

        return juce::Rectangle<int> (
            (getWidth()  - w) / 2,
            (getHeight() - h) / 2,
            w, h);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MessageOverlay)
};
