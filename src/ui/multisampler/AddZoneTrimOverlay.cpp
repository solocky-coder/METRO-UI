#include "AddZoneTrimOverlay.h"
#include "../UIHelpers.h"
#include "../../audio/SampleData.h"
#include <cmath>

// =============================================================================
//  Background decode job
//  ─────────────────────────────────────────────────────────────────────────
//  Deliberately does NOT go through SampleData::decodeFromFile at the
//  project's sample rate — that resamples, which would shift every frame
//  number relative to the file sfizz will actually read at export time. We
//  read the reader's own native rate first, then decode at that same rate
//  so decodeFromFile's resample branch never triggers, while still getting
//  its existing corruption/format/duration guards and stereo up-mix for
//  free rather than re-implementing them here.
// =============================================================================
class AddZoneTrimOverlay::DecodeJob : public juce::ThreadPoolJob
{
public:
    DecodeJob (juce::File fileToDecode,
               juce::Component::SafePointer<AddZoneTrimOverlay> ownerToNotify,
               std::shared_ptr<std::atomic<bool>> cancelFlag)
        : juce::ThreadPoolJob ("AddZoneTrimOverlay::DecodeJob"),
          file (std::move (fileToDecode)),
          owner (ownerToNotify),
          cancelled (std::move (cancelFlag))
    {
    }

    JobStatus runJob() override
    {
        if (cancelled->load (std::memory_order_relaxed))
            return jobHasFinished;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        double nativeRate = 0.0;

        {
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            {
                notifyFailure ("Couldn't read this file.");
                return jobHasFinished;
            }
            nativeRate = reader->sampleRate;
        }

        // Native rate in, native rate requested out -> decodeFromFile's
        // resample branch (abs(source - project) > 0.01) never triggers.
        auto decoded = SampleData::decodeFromFile (file, nativeRate);
        if (decoded == nullptr)
        {
            notifyFailure ("Couldn't decode this file.");
            return jobHasFinished;
        }

        if (cancelled->load (std::memory_order_relaxed))
            return jobHasFinished;

        const auto totalFrames = (int64_t) decoded->buffer.getNumSamples();

        std::vector<float> peakMax ((size_t) kNumDisplayPeaks, 0.0f);
        std::vector<float> peakMin ((size_t) kNumDisplayPeaks, 0.0f);
        buildDisplayPeaks (decoded->buffer, totalFrames, peakMax, peakMin);

        auto ownerCopy = owner;
        auto cancelledCopy = cancelled;
        juce::MessageManager::callAsync (
            [ownerCopy, cancelledCopy, totalFrames, nativeRate,
             peakMax = std::move (peakMax), peakMin = std::move (peakMin)] () mutable
            {
                if (cancelledCopy->load (std::memory_order_relaxed))
                    return;
                if (auto* o = ownerCopy.getComponent())
                    o->handleDecodeSuccess (totalFrames, nativeRate,
                                            std::move (peakMax), std::move (peakMin));
            });

        return jobHasFinished;
    }

private:
    static void buildDisplayPeaks (const juce::AudioBuffer<float>& buffer, int64_t totalFrames,
                                    std::vector<float>& peakMax, std::vector<float>& peakMin)
    {
        const int numChannels = buffer.getNumChannels();
        if (numChannels <= 0 || totalFrames <= 0)
            return;

        const int numBuckets = (int) peakMax.size();
        for (int b = 0; b < numBuckets; ++b)
        {
            const int64_t bucketStart = (totalFrames * b) / numBuckets;
            const int64_t bucketEnd   = juce::jmax (bucketStart + 1, (totalFrames * (b + 1)) / numBuckets);

            float mx = 0.0f, mn = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* data = buffer.getReadPointer (ch);
                for (int64_t i = bucketStart; i < bucketEnd; ++i)
                {
                    mx = juce::jmax (mx, data[i]);
                    mn = juce::jmin (mn, data[i]);
                }
            }
            peakMax[(size_t) b] = mx;
            peakMin[(size_t) b] = mn;
        }
    }

    void notifyFailure (const juce::String& message)
    {
        auto ownerCopy = owner;
        auto cancelledCopy = cancelled;
        juce::MessageManager::callAsync ([ownerCopy, cancelledCopy, message]
        {
            if (cancelledCopy->load (std::memory_order_relaxed))
                return;
            if (auto* o = ownerCopy.getComponent())
                o->handleDecodeFailure (message);
        });
    }

    juce::File file;
    juce::Component::SafePointer<AddZoneTrimOverlay> owner;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

// =============================================================================
//  AddZoneTrimOverlay
// =============================================================================

AddZoneTrimOverlay::AddZoneTrimOverlay (const juce::File& sampleFile, juce::ThreadPool& decodePool)
    : file (sampleFile), pool (decodePool)
{
    const auto& T = getTheme();

    titleLabel.setText ("TRIM  —  " + sampleFile.getFileName().toUpperCase(), juce::dontSendNotification);
    titleLabel.setFont (DysektLookAndFeel::makeFont (16.0f, true));
    titleLabel.setColour (juce::Label::textColourId, T.accent);
    titleLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (titleLabel);

    readoutLabel.setFont (DysektLookAndFeel::makeFont (11.5f));
    readoutLabel.setColour (juce::Label::textColourId, T.foreground.withAlpha (0.75f));
    readoutLabel.setJustificationType (juce::Justification::centred);
    readoutLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (readoutLabel);

    UIHelpers::styleSecondaryPopupButton (resetBtn, T);
    resetBtn.onClick = [this] { resetTrim(); repaint(); };
    addAndMakeVisible (resetBtn);

    UIHelpers::styleSecondaryPopupButton (cancelBtn, T);
    cancelBtn.onClick = [this] { fire (false); };
    addAndMakeVisible (cancelBtn);

    UIHelpers::stylePrimaryPopupButton (nextBtn, T);
    nextBtn.onClick = [this] { fire (true); };
    nextBtn.setEnabled (false);   // enabled once decode succeeds
    addAndMakeVisible (nextBtn);

    setInterceptsMouseClicks (true, true);
    setMouseCursor (juce::MouseCursor::NormalCursor);
    for (auto* b : { &resetBtn, &cancelBtn, &nextBtn })
        b->setMouseCursor (juce::MouseCursor::NormalCursor);

    pool.addJob (new DecodeJob (sampleFile,
                                 juce::Component::SafePointer<AddZoneTrimOverlay> (this),
                                 cancelled),
                 true);
}

AddZoneTrimOverlay::~AddZoneTrimOverlay()
{
    // The DecodeJob may still be running or queued behind other jobs on the
    // shared pool. Flip the flag so any in-flight callAsync it posts after
    // we're gone is a no-op even before it re-checks the (now possibly
    // reused) SafePointer -- belt-and-suspenders lifetime safety per the
    // implementation notes' async-decode section.
    cancelled->store (true, std::memory_order_relaxed);
}

// ── Decode callbacks (message thread) ───────────────────────────────────────

void AddZoneTrimOverlay::handleDecodeSuccess (int64_t newTotalFrames, double newSourceSampleRate,
                                               std::vector<float> peakMax, std::vector<float> peakMin)
{
    decoding         = false;
    decodeFailed     = false;
    totalFrames      = newTotalFrames;
    sourceSampleRate = newSourceSampleRate;
    peaksMax         = std::move (peakMax);
    peaksMin         = std::move (peakMin);

    resetTrim();   // seeds trimStart/trimEnd to the full file
    nextBtn.setEnabled (true);
    repaint();
}

void AddZoneTrimOverlay::handleDecodeFailure (const juce::String& message)
{
    decoding     = false;
    decodeFailed = true;
    errorMessage = message;
    nextBtn.setEnabled (false);
    repaint();
}

void AddZoneTrimOverlay::resetTrim()
{
    trimStart = 0;
    trimEnd   = totalFrames;

    // Covers both call sites: the initial seed from handleDecodeSuccess()
    // (first time there's anything playable) and the RESET button.
    if (onTrimChanged)
        onTrimChanged (trimStart, trimEnd);
}

// ── Layout ───────────────────────────────────────────────────────────────

juce::Rectangle<int> AddZoneTrimOverlay::dialogBox() const
{
    const int w = juce::jmin (640, getWidth()  - 40);
    const int h = juce::jmin (340, getHeight() - 40);
    return { (getWidth() - w) / 2, (getHeight() - h) / 2, w, h };
}

juce::Rectangle<int> AddZoneTrimOverlay::waveformArea() const
{
    const auto box = dialogBox();
    const int padX = 18;
    return { box.getX() + padX, box.getY() + 54, box.getWidth() - padX * 2, 140 };
}

void AddZoneTrimOverlay::resized()
{
    const auto box = dialogBox();
    const int padX = 18;

    titleLabel.setBounds (box.getX() + padX, box.getY() + 14, box.getWidth() - padX * 2, 24);

    const auto wave = waveformArea();
    readoutLabel.setBounds (wave.getX(), wave.getBottom() + 12, wave.getWidth(), 20);

    const int btnH  = 34;
    const int btnW  = 120;
    const int gap   = 12;
    const int btnY  = box.getBottom() - btnH - 16;
    const int totalW = btnW * 3 + gap * 2;
    const int btnX  = box.getCentreX() - totalW / 2;

    resetBtn .setBounds (btnX,                    btnY, btnW, btnH);
    cancelBtn.setBounds (btnX + btnW + gap,        btnY, btnW, btnH);
    nextBtn  .setBounds (btnX + (btnW + gap) * 2,  btnY, btnW, btnH);
}

// ── Frame <-> pixel mapping ──────────────────────────────────────────────

int64_t AddZoneTrimOverlay::pixelToFrame (int px) const
{
    const auto wave = waveformArea();
    if (wave.getWidth() <= 0 || totalFrames <= 0)
        return 0;
    const float frac = juce::jlimit (0.0f, 1.0f,
                                      (float) (px - wave.getX()) / (float) wave.getWidth());
    return (int64_t) (frac * (float) totalFrames);
}

int AddZoneTrimOverlay::frameToPixel (int64_t frame) const
{
    const auto wave = waveformArea();
    if (totalFrames <= 0)
        return wave.getX();
    const float frac = (float) frame / (float) totalFrames;
    return wave.getX() + (int) (frac * (float) wave.getWidth());
}

juce::String AddZoneTrimOverlay::formatTime (int64_t frame) const
{
    const double seconds = sourceSampleRate > 0.0 ? (double) frame / sourceSampleRate : 0.0;
    const int mins = (int) (seconds / 60.0);
    const double secs = seconds - mins * 60.0;
    return juce::String::formatted ("%d:%05.2f", mins, secs);
}

// ── Paint ────────────────────────────────────────────────────────────────

void AddZoneTrimOverlay::paint (juce::Graphics& g)
{
    const auto& T = getTheme();

    UIHelpers::drawPopupBackdrop (g, getLocalBounds());

    const auto box  = dialogBox();
    UIHelpers::drawPopupBox (g, box, T);

    const auto wave = waveformArea();

    g.setColour (T.waveformBg);
    g.fillRect (wave);
    g.setColour (T.separator);
    g.drawHorizontalLine (wave.getCentreY(), (float) wave.getX(), (float) wave.getRight());

    if (decoding)
    {
        g.setFont (DysektLookAndFeel::makeFont (13.0f));
        g.setColour (T.foreground.withAlpha (0.6f));
        g.drawText ("Decoding preview...", wave, juce::Justification::centred, false);
        return;
    }

    if (decodeFailed)
    {
        g.setFont (DysektLookAndFeel::makeFont (13.0f));
        g.setColour (juce::Colour (0xFFCC4444));
        g.drawText (errorMessage, wave, juce::Justification::centred, false);
        return;
    }

    // ── Waveform ─────────────────────────────────────────────────────────
    const int cy = wave.getCentreY();
    const float scale = (float) wave.getHeight() * 0.45f;
    const int numPeaks = juce::jmin ((int) peaksMax.size(), wave.getWidth());

    if (numPeaks > 0)
    {
        juce::Path fillPath;
        fillPath.startNewSubPath ((float) wave.getX(), (float) cy - peaksMax[0] * scale);
        for (int px = 1; px < numPeaks; ++px)
        {
            const size_t idx = (size_t) ((int64_t) px * (int64_t) peaksMax.size() / numPeaks);
            fillPath.lineTo ((float) (wave.getX() + px), (float) cy - peaksMax[idx] * scale);
        }
        for (int px = numPeaks - 1; px >= 0; --px)
        {
            const size_t idx = (size_t) ((int64_t) px * (int64_t) peaksMin.size() / numPeaks);
            fillPath.lineTo ((float) (wave.getX() + px), (float) cy - peaksMin[idx] * scale);
        }
        fillPath.closeSubPath();

        g.setColour (T.waveform);
        g.fillPath (fillPath);
    }

    // ── Shade outside the active trim region, draw IN/OUT handles ────────
    const int x1 = juce::jlimit (wave.getX(), wave.getRight(), frameToPixel (trimStart));
    const int x2 = juce::jlimit (x1, wave.getRight(), frameToPixel (trimEnd));

    g.setColour (juce::Colours::black.withAlpha (0.55f));
    if (x1 > wave.getX())     g.fillRect (wave.getX(), wave.getY(), x1 - wave.getX(), wave.getHeight());
    if (x2 < wave.getRight()) g.fillRect (x2, wave.getY(), wave.getRight() - x2, wave.getHeight());

    g.setColour (T.accent.withAlpha (0.9f));
    g.drawVerticalLine (x1, (float) wave.getY(), (float) wave.getBottom());
    g.drawVerticalLine (x2, (float) wave.getY(), (float) wave.getBottom());

    // Flat metro tab handles — square accent tile with a two-bar grip cut
    // out in the waveform background colour, rather than the previous
    // pointed triangle flags (the only skeuomorphic shape in this overlay;
    // every other draggable/interactive element in the app — zone tiles,
    // transport buttons, LCD sliders — is a flat square-cornered tile).
    // IN's tab sits to the right of its line (grabbing rightward, toward
    // the kept audio); OUT's sits to the left, mirroring that same
    // "toward the kept region" orientation.
    drawTrimHandleTab (g, x1, wave.getY(), true);
    drawTrimHandleTab (g, x2, wave.getY(), false);

    // ── Readout ────────────────────────────────────────────────────────
    const auto duration = trimEnd - trimStart;
    readoutLabel.setText (
        "IN " + juce::String (trimStart) + " (" + formatTime (trimStart) + ")"
        + "   OUT " + juce::String (trimEnd) + " (" + formatTime (trimEnd) + ")"
        + "   LENGTH " + juce::String (duration) + " frames",
        juce::dontSendNotification);
}

void AddZoneTrimOverlay::drawTrimHandleTab (juce::Graphics& g, int lineX, int topY, bool tabOnRight)
{
    const auto& T = getTheme();

    constexpr int tabW = 16;
    constexpr int tabH = 14;
    const int tabX = tabOnRight ? lineX : lineX - tabW;

    g.setColour (T.accent);
    g.fillRect (tabX, topY, tabW, tabH);

    // Two-bar grip, cut out in the waveform's own background colour so it
    // reads as a notch in the tab rather than a separate drawn element.
    g.setColour (T.waveformBg);
    const int gripY = topY + 4;
    const int gripH = tabH - 8;
    g.fillRect (tabX + tabW / 2 - 3, gripY, 2, gripH);
    g.fillRect (tabX + tabW / 2 + 1, gripY, 2, gripH);
}

// ── Mouse handling ───────────────────────────────────────────────────────

void AddZoneTrimOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! dialogBox().contains (e.getPosition()))
    {
        fire (false);
        return;
    }

    if (decoding || decodeFailed)
        return;

    const int xIn  = frameToPixel (trimStart);
    const int xOut = frameToPixel (trimEnd);

    if (std::abs (e.x - xIn) <= kHandleHitPx)
        activeDrag = Handle::in;
    else if (std::abs (e.x - xOut) <= kHandleHitPx)
        activeDrag = Handle::out;
    else
        activeDrag = Handle::none;
}

void AddZoneTrimOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (activeDrag == Handle::none || totalFrames <= 0)
        return;

    const int64_t frame = juce::jlimit ((int64_t) 0, totalFrames, pixelToFrame (e.x));

    if (activeDrag == Handle::in)
        trimStart = juce::jlimit ((int64_t) 0, trimEnd - 1, frame);
    else
        trimEnd = juce::jlimit (trimStart + 1, totalFrames, frame);

    if (onTrimChanged)
        onTrimChanged (trimStart, trimEnd);

    repaint();
}

void AddZoneTrimOverlay::mouseUp (const juce::MouseEvent&)
{
    activeDrag = Handle::none;
}

void AddZoneTrimOverlay::mouseMove (const juce::MouseEvent& e)
{
    if (decoding || decodeFailed)
        return;

    const int xIn  = frameToPixel (trimStart);
    const int xOut = frameToPixel (trimEnd);

    Handle newHover = Handle::none;
    if (std::abs (e.x - xIn) <= kHandleHitPx)       newHover = Handle::in;
    else if (std::abs (e.x - xOut) <= kHandleHitPx) newHover = Handle::out;

    if (newHover != hoveredHandle)
    {
        hoveredHandle = newHover;
        setMouseCursor (newHover == Handle::none ? juce::MouseCursor::NormalCursor
                                                  : juce::MouseCursor::LeftRightResizeCursor);
    }
}

// ── Result ───────────────────────────────────────────────────────────────

void AddZoneTrimOverlay::fire (bool confirmed)
{
    if (! onResult)
        return;

    Result r;
    r.start       = trimStart;
    r.end         = trimEnd;
    r.totalFrames = totalFrames;
    // NEXT is disabled while decodeFailed/decoding, but guard anyway in case
    // a caller wires something else to onResult directly.
    onResult (r, confirmed && ! decodeFailed && ! decoding);
}
