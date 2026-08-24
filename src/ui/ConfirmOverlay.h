#pragma once
#include "UIHelpers.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

class ConfirmOverlay : public juce::Component
{
public:
    std::function<void(bool)> onResult;

    ConfirmOverlay (const juce::String& title,
                    const juce::String& message,
                    const juce::String& yesText,
                    const juce::String& noText)
        : titleText (title), messageText (message)
    {
        yesBtn.setButtonText (yesText);
        noBtn .setButtonText (noText);

        const auto& T = getTheme();
        UIHelpers::stylePrimaryPopupButton   (yesBtn, T);
        UIHelpers::styleSecondaryPopupButton (noBtn,  T);

        // yesBtn/noBtn.onClick fire synchronously from inside juce::Button's
        // own click-handling code (its mouseUp()), which is a member
        // function running on yesBtn/noBtn themselves. Every caller's
        // onResult handler destroys `this` ConfirmOverlay immediately
        // (confirmOverlay.reset()) — which also destroys yesBtn/noBtn — so
        // calling onResult() directly here would free the button out from
        // under its own still-running click handler (crashes on either
        // button, since both go through this). Deferring via callAsync runs
        // onResult() as a fresh top-level callback after that call stack has
        // fully unwound, so it's safe for the receiver to destroy us then.
        // SafePointer guards the (unlikely, but possible with a fast
        // double-click) case where we're already gone by the time it fires.
        juce::Component::SafePointer<ConfirmOverlay> safeThis (this);
        yesBtn.onClick = [safeThis]
        {
            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis != nullptr && safeThis->onResult)
                {
                    auto cb = std::move (safeThis->onResult);
                    cb (true);
                }
            });
        };
        noBtn.onClick = [safeThis]
        {
            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis != nullptr && safeThis->onResult)
                {
                    auto cb = std::move (safeThis->onResult);
                    cb (false);
                }
            });
        };

        addAndMakeVisible (yesBtn);
        addAndMakeVisible (noBtn);

        setInterceptsMouseClicks (true, true);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& T = getTheme();

        UIHelpers::drawPopupBackdrop (g, getLocalBounds());

        const auto box = dialogBox();
        UIHelpers::drawPopupBox (g, box, T);

        const int padX = 18;
        g.setFont (DysektLookAndFeel::makeFont (15.0f, true));
        g.setColour (T.accent);
        g.drawText (titleText,
                    box.getX() + padX, box.getY() + 14,
                    box.getWidth() - padX * 2, 20,
                    juce::Justification::centredLeft, false);

        // Fixed, always-legible size, wrapped across as many lines as it
        // needs — dialogBox() below grows to fit instead of this getting
        // cut off with an ellipsis at a fixed one-line height. See
        // UIHelpers::measureWrappedTextHeight/drawWrappedText's comment.
        UIHelpers::drawWrappedText (g, messageText, bodyFont(), T.foreground.withAlpha (0.85f),
                    juce::Rectangle<float> ((float) (box.getX() + padX), (float) (box.getY() + 46),
                                             (float) (box.getWidth() - padX * 2),
                                             (float) messageHeight()));
    }

    void resized() override
    {
        const auto box  = dialogBox();
        const int btnW  = 90;
        const int btnH  = 28;
        const int gap   = 10;
        const int btnY  = box.getBottom() - btnH - 14;
        const int totalBtnW = btnW * 2 + gap;
        const int btnX  = box.getCentreX() - totalBtnW / 2;

        yesBtn.setBounds (btnX,           btnY, btnW, btnH);
        noBtn .setBounds (btnX + btnW + gap, btnY, btnW, btnH);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! dialogBox().contains (e.getPosition()))
        {
            // Same reasoning as the constructor's yesBtn/noBtn.onClick fix —
            // this runs from inside our own mouseDown(), so onResult()
            // (which the caller uses to destroy us) must be deferred past
            // this call stack, not invoked directly from here.
            juce::Component::SafePointer<ConfirmOverlay> safeThis (this);
            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis != nullptr && safeThis->onResult)
                {
                    auto cb = std::move (safeThis->onResult);
                    cb (false);
                }
            });
        }
    }

private:
    juce::String titleText, messageText;
    juce::TextButton yesBtn, noBtn;

    static juce::Font bodyFont() { return DysektLookAndFeel::makeFont (12.0f); }

    // Wrapped text height at the box's actual text width — shared by
    // dialogBox() (to size the box) and paint() (to size the draw area),
    // computed once per call rather than cached since messageText/width
    // only change on construction/resize, both infrequent for a modal popup.
    int messageHeight() const
    {
        const int padX = 18;
        const int w = juce::jmin (400, getWidth() - 40);
        const int textWidth = juce::jmax (10, w - padX * 2);
        return UIHelpers::measureWrappedTextHeight (messageText, bodyFont(), textWidth);
    }

    juce::Rectangle<int> dialogBox() const
    {
        const int w = juce::jmin (400, getWidth() - 40);

        // 46 = top offset of the message text (matches paint()); 14 = gap
        // between text and button row; 28 = button height; 14 = bottom margin
        // (matches resized()'s btnY, 14px above the box bottom).
        const int contentH = 46 + messageHeight() + 14 + 28 + 14;

        const int maxH = juce::jmax (148, getHeight() - 40);
        const int h    = juce::jlimit (148, maxH, contentH);

        return juce::Rectangle<int> (
            (getWidth()  - w) / 2,
            (getHeight() - h) / 2,
            w, h);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConfirmOverlay)
};
