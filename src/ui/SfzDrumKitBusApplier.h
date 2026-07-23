#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"

//==============================================================================
//  SfzDrumKitBusApplier
//
//  Fire-and-forget helper used after the user confirms the "this looks like
//  a drum kit — auto-assign outputs?" prompt (see SfzLayoutClassifier.h and
//  PluginEditor's browserPanel.onLoadRequest, uiMode == 1 branch).
//
//  loadSoundFontAsync() means sliceManager2's real slices don't exist yet
//  at the moment the user confirms the prompt — they're created shortly
//  after, on the audio thread, once the background load completes. This
//  polls the thread-safe published snapshot (getUiSliceSnapshot2(), the
//  same one the SFZ-PLAYER UI itself reads from) until the slice count
//  matches what was classified, then pushes one CmdSelectSlice +
//  CmdSetSliceParam(FieldOutputBus) pair per slice through the command
//  queue — the only thread-safe way to mutate sliceManager2 from here.
//
//  Gives up silently after ~3s (load never finished, or failed) rather
//  than ever guessing and assigning bus numbers against the wrong file's
//  slices. Also gives up if the slice count doesn't match by the time it
//  succeeds falls out of range for another reason (e.g. the user loaded a
//  different .sfz into the same tab while the prompt was still up) --
//  correctness over persistence.
//==============================================================================
class SfzDrumKitBusApplier : private juce::Timer
{
public:
    SfzDrumKitBusApplier (DysektProcessor& p, int expectedZoneCount)
        : processor (p), expected (expectedZoneCount)
    {
        startTimer (100);
    }

private:
    void timerCallback() override
    {
        if (++attempts > kMaxAttempts)
        {
            stopTimer();
            delete this;
            return;
        }

        const auto& snap = processor.getUiSliceSnapshot2();
        const int numSlices = snap.numSlices;
        processor.releaseUiSliceSnapshot2();

        if (numSlices != expected)
            return;   // still loading (or a mismatched load raced us) — keep waiting

        stopTimer();

        for (int i = 0; i < expected; ++i)
        {
            DysektProcessor::Command sel;
            sel.type          = DysektProcessor::CmdSelectSlice;
            sel.targetEngine2 = true;
            sel.intParam1     = i;
            processor.pushCommand (sel);

            DysektProcessor::Command set;
            set.type          = DysektProcessor::CmdSetSliceParam;
            set.targetEngine2 = true;
            set.intParam1     = DysektProcessor::FieldOutputBus;
            // Buses 1-15 (bus 0 = Main is left alone; FieldOutputBus's handler
            // clamps to 0-15 anyway, so this is always in range). Wraps if
            // there are more than 15 zones -- no UI currently exists to pick
            // specific buses per zone, so simple round-robin distribution is
            // the reasonable default; the mixer's existing per-slice OUT
            // cell doesn't apply here since SFZ-PLAYER has no per-slice rows
            // (single summary row -- see the mixer parity work above).
            set.floatParam1  = (float) (1 + (i % 15));
            processor.pushCommand (set);
        }

        delete this;
    }

    DysektProcessor& processor;
    int expected;
    int attempts { 0 };
    static constexpr int kMaxAttempts = 30;   // 30 * 100ms = 3s
};
