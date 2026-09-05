#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// ─────────────────────────────────────────────────────────────────────────────
//  ContextMenuButton
//
//  A juce::TextButton that treats a right-click (or other platforms'
//  "popup menu" modifier) as a distinct gesture from a normal click, instead
//  of both firing the same onClick/onStateChange path the way a plain
//  TextButton does by default.
//
//  Deliberately header-only and dependency-free (just juce_gui_basics) so it
//  can be shared between src/ui and src/metro without either pulling in the
//  other's heavier UI helpers (ThemeData, DysektLookAndFeel, etc).
//
//  Usage:
//    ContextMenuButton linkBtn { "LINK" };
//    linkBtn.onRightClick = [this] (const juce::MouseEvent&)
//    {
//        juce::PopupMenu m;
//        m.addItem (1, "Follow Remote Start/Stop", true, engine.getLinkFollowsTransport());
//        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (linkBtn),
//            [this] (int result) { if (result == 1)
//                engine.setLinkFollowsTransport (! engine.getLinkFollowsTransport()); });
//    };
//    // linkBtn.onClick / onStateChange keep working exactly as before for
//    // left-clicks — only the right-click path is new.
// ─────────────────────────────────────────────────────────────────────────────
class ContextMenuButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    /** Fired instead of the normal click path when the mouse-down is a
     *  right-click (or platform-equivalent popup-menu gesture). Left-clicks
     *  are untouched — they still flow through TextButton::mouseDown() as
     *  usual, so onClick/onStateChange/setClickingTogglesState all keep
     *  working unmodified. */
    std::function<void (const juce::MouseEvent&)> onRightClick;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            // Deliberately don't forward to TextButton::mouseDown() here —
            // doing so would also flip the toggle state / queue a click for
            // mouseUp, which reads as "the button was pressed" (visually and
            // via onStateChange) for what's meant to be a menu-only gesture.
            if (onRightClick != nullptr)
                onRightClick (e);
            return;
        }
        juce::TextButton::mouseDown (e);
    }
};
