#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/MidiClip.h"

//==============================================================================
//  VelocityLane
//
//  Draws a bar graph of note velocities and lets the user drag them.
//  Communicates with PianoRollComponent via a shared reference to the clip
//  and a callback that the piano roll calls whenever the clip changes.
//==============================================================================
class VelocityLane : public juce::Component
{
public:
    //  The piano roll sets these so we know how to map ticks → pixels.
    double pixelsPerTick  = 0.2;
    double scrollOffsetX  = 0.0;  // in pixels (same as piano roll horizontal scroll)

    VelocityLane() = default;

    void setClip (MidiClip* c)
    {
        clip = c;
        repaint();
    }

    void setSelectedNote (int index)
    {
        selectedNote = index;
        repaint();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bg = juce::Colour (0xFF0E1219);
        g.fillAll (bg);

        // A labelled lane reads as an editing control rather than stray blocks.
        g.setColour (juce::Colour (0xFF2A3440));
        g.fillRect (getLocalBounds().removeFromTop (1));
        g.setFont (juce::Font (9.0f, juce::Font::bold));
        g.setColour (juce::Colour (0xFF8EA5B8));
        g.drawText ("VELOCITY", 7, 4, 64, 14, juce::Justification::centredLeft, false);
        g.setFont (juce::Font (8.0f));
        g.setColour (juce::Colour (0xFF5F7182));
        g.drawText ("127", getWidth() - 28, 4, 22, 12, juce::Justification::centredRight, false);
        g.drawText ("0",   getWidth() - 18, getHeight() - 14, 12, 10, juce::Justification::centredRight, false);
        g.setColour (juce::Colour (0xFF222C37));
        g.drawHorizontalLine (getHeight() / 2, 0.f, (float) getWidth());

        if (clip == nullptr) return;

        const juce::ScopedReadLock sl (clip->getLock());
        const auto& notes = clip->getNotes();
        const float h = (float) getHeight() - 7.f;

        for (int i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes.getReference (i);
            const float x = (float)(n.startTick * pixelsPerTick - scrollOffsetX);
            const float barH = juce::jmax (2.f, h * (n.velocity / 127.f));
            const float barW = juce::jmax (2.f, (float)(n.durationTick * pixelsPerTick) - 1.f);

            const bool sel = (i == selectedNote);
            g.setColour (sel ? juce::Colour::fromFloatRGBA (0.35f, 0.88f, 0.84f, 1.0f)
                             : juce::Colour::fromFloatRGBA (0.35f, 0.60f, 0.76f, 0.82f));

            g.fillRect (juce::Rectangle<float> (x, (float)(getHeight()) - barH - 2.f, barW, barH));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragNoteIdx = hitTestBar (e.x);
        if (dragNoteIdx >= 0)
        {
            dragStartY = e.y;
            if (clip)
            {
                const juce::ScopedReadLock sl (clip->getLock());
                dragStartVel = clip->getNotes()[dragNoteIdx].velocity;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragNoteIdx < 0 || clip == nullptr) return;
        const float dy = (float)(dragStartY - e.y);
        const int newVel = juce::jlimit (1, 127,
            dragStartVel + (int)(dy * 127.f / (float)getHeight()));
        clip->setNoteVelocity (dragNoteIdx, newVel);
        if (onVelocityChanged) onVelocityChanged (dragNoteIdx, newVel);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragNoteIdx = -1; }

    std::function<void(int noteIdx, int velocity)> onVelocityChanged;

private:
    MidiClip* clip = nullptr;
    int  selectedNote  = -1;
    int  dragNoteIdx   = -1;
    int  dragStartY    = 0;
    int  dragStartVel  = 100;

    int hitTestBar (int mouseX) const
    {
        if (clip == nullptr) return -1;
        const juce::ScopedReadLock sl (clip->getLock());
        const auto& notes = clip->getNotes();
        for (int i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes.getReference (i);
            const float x = (float)(n.startTick * pixelsPerTick - scrollOffsetX);
            const float w = juce::jmax (2.f, (float)(n.durationTick * pixelsPerTick) - 1.f);
            if (mouseX >= x && mouseX <= x + w) return i;
        }
        return -1;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelocityLane)
};
