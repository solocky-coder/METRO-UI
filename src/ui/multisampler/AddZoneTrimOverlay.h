#pragma once
// =============================================================================
//  AddZoneTrimOverlay.h — Add Zone step 1: non-destructive sample trim
//  ─────────────────────────────────────────────────────────────────────────
//  Shown immediately after the user picks a sample file in the Add Zone
//  flow, before AddZoneOverlay's lo/hi/root key step. Lets the user drag
//  IN/OUT handles over a waveform preview of the chosen file and pick the
//  frame range that becomes the new zone's SampleZone::sampleStart/sampleEnd.
//
//  Deliberately self-contained: this component owns only its own preview
//  buffer and trim state. It never touches DysektProcessor, sliceManager2,
//  or any live sample/slice state — see the class-level rationale in the
//  METRO-UI Multisampler Add Zone Trim implementation notes for why this
//  must not reuse WaveformView's trim fields (processor.trimRegionStart/End,
//  processor.sampleData): those belong to the unrelated global Slicer trim
//  workflow. The caller (MultisamplerEditor) hands this component a sample
//  file and a background juce::ThreadPool to decode on (in practice
//  processor.fileLoadPool, borrowed purely as a worker queue) — everything
//  else lives here.
//
//  Frame domain: the decoded preview is read at the FILE'S OWN native
//  sample rate (see AddZoneTrimOverlay.cpp's decode job), not the project
//  rate. sfizz's offset/end opcodes address frames in the source file
//  itself, and so does SampleZone::sampleStart/sampleEnd — if this preview
//  resampled to the project rate (the way SampleData::decodeFromFile
//  normally does for the live sampler), the frame numbers the user drags to
//  would silently disagree with what ends up in the zone and what sfizz
//  actually plays. Reading at the native rate keeps both in the same
//  domain.
//
//  Endpoint convention: matches SampleZone.h — Result::end is EXCLUSIVE
//  (one past the last played frame), so `[start, end)`. RESET restores the
//  full file, i.e. { 0, totalFrames }.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <atomic>
#include "../DysektLookAndFeel.h"
#include "../../audio/SampleData.h"

class AddZoneTrimOverlay : public juce::Component,
                           private juce::MidiKeyboardStateListener
{
public:
    struct Result
    {
        int64_t start       = 0;
        int64_t end         = 0;   // exclusive — [start, end)
        int64_t totalFrames = 0;
    };

    /** @param sampleFile        file already chosen by the caller's file
     *                           picker — this class never shows its own
     *                           file chooser.
     *  @param decodePool        background pool the preview decode job runs
     *                           on. Pass DysektProcessor::fileLoadPool; this
     *                           class only ever uses it as a plain
     *                           juce::ThreadPool and never reaches back into
     *                           the processor otherwise.
     *  @param initialTrimStart  seeds the IN handle once decode succeeds —
     *                           lets a caller reopen this on an *existing*
     *                           zone's current SampleZone::sampleStart
     *                           instead of always starting at the top of the
     *                           file. Ignored (clamped away) if out of range
     *                           for the decoded file. Default 0 matches the
     *                           Add Zone flow's "nothing trimmed yet" start.
     *  @param initialTrimEnd    seeds the OUT handle the same way, from an
     *                           existing zone's SampleZone::sampleEnd — same
     *                           "-1 == full sample length" convention as
     *                           that field (see SampleZone.h) rather than
     *                           requiring the caller to already know
     *                           totalFrames before decode has even run.
     *                           Default -1 matches the Add Zone flow's
     *                           "whole file" starting selection. Note the
     *                           RESET button always restores the full file
     *                           regardless of these seeds — it's a "start
     *                           over" control, not an "undo to seed". */
    AddZoneTrimOverlay (const juce::File& sampleFile, juce::ThreadPool& decodePool,
                        int64_t initialTrimStart = 0, int64_t initialTrimEnd = -1);
    ~AddZoneTrimOverlay() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** confirmed == false → user cancelled (or clicked outside the dialog);
     *  do not create a zone. Never fires with confirmed == true while the
     *  preview failed to decode — NEXT is disabled in that state. */
    std::function<void (Result, bool confirmed)> onResult;

    /** Fired whenever the live [start, end) trim region changes: on every
     *  handle drag step, on RESET, and once right after a successful decode
     *  (seeded from initialTrimStart/initialTrimEnd above, i.e. the full
     *  file unless the caller passed something else). Lets the caller keep
     *  an audition preview in sync with the region the user is currently
     *  dragging, without waiting for NEXT/onResult. Never fires while
     *  decoding or after a decode failure — there is nothing playable yet
     *  in either case. */
    std::function<void (int64_t start, int64_t end)> onTrimChanged;

    /** Fired once, right after a successful decode, with the full decoded
     *  buffer (not just the display peaks kept for the waveform) so the
     *  caller can publish it somewhere real-time-readable for chromatic
     *  audition -- see PluginProcessor::trimAuditionSample. Never fires on
     *  decode failure. The SnapshotPtr is already an immutable, fully-built
     *  payload (see SampleData::applyDecodedSample()'s doc comment) built
     *  on the decode job's worker thread, so handing it straight to
     *  applyDecodedSample() from the caller's message-thread lambda is
     *  real-time-safe with no further copying. */
    std::function<void (SampleData::SnapshotPtr decoded, double sourceSampleRate)> onSampleDecoded;

    /** Fired by the embedded on-screen keyboard below on every note-on
     *  (isOn == true) and note-off (isOn == false). The caller routes this
     *  into whatever plays the chromatic audition (see
     *  PluginProcessor::triggerTrimAuditionNote) -- this class only ever
     *  forwards the keyboard's own note events, it never touches playback
     *  itself, same separation as onTrimChanged/onSampleDecoded above. */
    std::function<void (int note, bool isOn)> onAuditionNote;

    // Called by the background decode job via MessageManager::callAsync —
    // public only so the .cpp's ThreadPoolJob subclass can reach them;
    // not part of the public API a caller should use.
    void handleDecodeSuccess (int64_t totalFrames, double sourceSampleRate,
                               std::vector<float> peakMax, std::vector<float> peakMin,
                               SampleData::SnapshotPtr decodedForAudition);
    void handleDecodeFailure (const juce::String& errorMessage);

private:
    static constexpr int kNumDisplayPeaks = 1024;
    static constexpr int kHandleHitPx     = 10;
    static constexpr int kKeyboardHeight  = 56;

    class DecodeJob;

    // juce::MidiKeyboardStateListener — forwards to onAuditionNote. This is
    // the "on-screen virtual keyboard" half of chromatic auditioning; a
    // physical/DAW-routed MIDI keyboard reaches the audition voices a
    // different way entirely (PluginProcessor::processMidi(), while
    // trimAuditionActive is set) and never goes through this listener.
    void handleNoteOn  (juce::MidiKeyboardState*, int /*midiChannel*/, int midiNoteNumber, float /*velocity*/) override;
    void handleNoteOff (juce::MidiKeyboardState*, int /*midiChannel*/, int midiNoteNumber, float /*velocity*/) override;

    void fire (bool confirmed);
    void resetTrim();
    /** Flat metro-style drag handle: a square accent tab at the top of the
        IN/OUT line with a two-bar grip cut out in the waveform background
        colour — see paint()'s call site for why this replaced a triangle. */
    void drawTrimHandleTab (juce::Graphics& g, int lineX, int topY, bool tabOnRight);
    juce::Rectangle<int> dialogBox() const;
    juce::Rectangle<int> waveformArea() const;
    int64_t pixelToFrame (int px) const;
    int     frameToPixel (int64_t frame) const;
    juce::String formatTime (int64_t frame) const;

    juce::File file;
    juce::ThreadPool& pool;
    // Raw constructor args, resolved against totalFrames once decode
    // succeeds (see handleDecodeSuccess) — same resolveSampleRange()
    // convention SampleZone.h's own accessors use, so a -1 seedTrimEnd
    // means "whole file" instead of a literal frame count the caller would
    // otherwise need to already know.
    int64_t seedTrimStart = 0;
    int64_t seedTrimEnd   = -1;
    std::shared_ptr<std::atomic<bool>> cancelled { std::make_shared<std::atomic<bool>> (false) };

    bool decoding     = true;
    bool decodeFailed = false;
    juce::String errorMessage;

    int64_t totalFrames     = 0;
    double  sourceSampleRate = 44100.0;
    std::vector<float> peaksMax, peaksMin;

    int64_t trimStart = 0;
    int64_t trimEnd   = 0;   // exclusive

    enum class Handle { none, in, out };
    Handle activeDrag  = Handle::none;
    Handle hoveredHandle = Handle::none;

    juce::Label      titleLabel;
    juce::Label      readoutLabel;
    juce::TextButton resetBtn  { "RESET" };
    juce::TextButton cancelBtn { "CANCEL" };
    juce::TextButton nextBtn   { "NEXT" };

    // Chromatic audition keyboard — always visible, even mid-decode (the
    // caller's playback side just has nothing to play yet until
    // onSampleDecoded fires, so early clicks are harmlessly silent). Root
    // for auditioning is always MIDI 60 regardless of whatever rootKey
    // gets chosen in the AddZoneOverlay step that follows — see
    // onAuditionNote's doc comment — so the keyboard is centred on that
    // note rather than on anything zone-specific.
    juce::MidiKeyboardState     auditionKeyboardState;
    juce::MidiKeyboardComponent auditionKeyboard { auditionKeyboardState,
                                                    juce::MidiKeyboardComponent::horizontalKeyboard };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddZoneTrimOverlay)
};
