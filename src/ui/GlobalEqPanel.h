#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

class DysektProcessor;

/**
 * GlobalEqPanel — interactive 5-band parametric EQ with Bode-style magnitude curve
 *                 and a real-time post-EQ spectrum analyser overlay.
 *
 * Bands:
 *   0  Low shelf     @ 80 Hz        (±18 dB)               — Y only
 *   1  Low-Mid peak  @ 100–1000 Hz  (±18 dB, Q 0.5–4)     — X + Y draggable
 *   2  Mid peak      @ 500–5000 Hz  (±18 dB, Q 0.5–4)     — X + Y draggable
 *   3  High-Mid peak @ 1–10 kHz     (±18 dB, Q 0.5–4)     — X + Y draggable
 *   4  High shelf    @ 12 kHz       (±18 dB)               — Y only
 *
 * Double-click a node to snap it back to 0 dB gain (and default frequency for
 * draggable bands).  All writes go through APVTS for undo / automation.
 *
 * The panel draws a theme-aware frame (gradient background, accent border)
 * matching the DualLcdControlFrame / MixerPanel visual language.
 *
 * Spectrum analyser:
 *   Reads post-EQ FFT magnitude data from DysektProcessor::spectrumAnalyser.
 *   Repaints at 30 Hz via juce::Timer.  The spectrum fill is drawn behind the
 *   EQ curve so the curve always reads clearly on top.
 */
class GlobalEqPanel : public juce::Component,
                      private juce::Timer
{
public:
    explicit GlobalEqPanel (DysektProcessor& p);
    ~GlobalEqPanel() override;

    void paint            (juce::Graphics&) override;
    void resized          () override;
    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    // ── Band indices ──────────────────────────────────────────────────────────
    enum { BLow = 0, BLowMid = 1, BMid = 2, BHighMid = 3, BHigh = 4, NoBand = -1 };
    static constexpr int kNumBands = 5;

    struct Node
    {
        int   band;
        float gainDb;        // ±kGainMax
        float freqHz;        // current frequency (fixed for shelves)
        float q;             // peak bands only
        float defaultFreqHz; // reset target on double-click
        bool  freqDraggable; // true for peak bands (1,2,3)
        juce::Point<float> pos;  // screen centre, updated in layout()
    };

    // ── Helpers ───────────────────────────────────────────────────────────────
    void  layout();
    juce::Rectangle<float> plotArea() const;

    float gainToY  (float dB) const;
    float yToGain  (float y)  const;
    float freqToX  (float hz) const;
    float xToFreq  (float x)  const;

    juce::Path buildCurve()    const;
    juce::Path buildSpectrum() const;   // ← new: spectrum analyser path

    static float filterMagnitudeAt (int type, float evalHz, float filterHz,
                                    float gainDb, float q, float sampleRate);

    int hitTest (juce::Point<float> p) const;  // returns band index or NoBand

    void resetBandToDefault (int band);

    // ── Timer ─────────────────────────────────────────────────────────────────
    void timerCallback() override;

    // ── State ─────────────────────────────────────────────────────────────────
    DysektProcessor& processor;

    std::array<Node, kNumBands> nodes;
    int   dragBand      = NoBand;
    float dragStartGain = 0.f;
    float dragStartFreq = 0.f;

    // ── Spectrum snapshot (UI thread only, smoothed copy of FFT data) ─────────
    // Size matches SpectrumAnalyser::numBins (1024).
    // We use a plain std::vector so we don't need to know fftSize at header time.
    std::vector<float> spectrumSmoothed;   // normalised 0..1
    double             spectrumSampleRate = 44100.0;

    static constexpr float kNodeRadius = 7.f;
    static constexpr float kGainMax    = 18.f;
    static constexpr float kSampleRate = 44100.f;

    // Frequency extents for the plot (log scale)
    static constexpr float kPlotFreqLo = 20.f;
    static constexpr float kPlotFreqHi = 20000.f;

    // Draggable frequency ranges per band
    static constexpr float kLowMidFreqLo  = 100.f,   kLowMidFreqHi  = 1000.f;
    static constexpr float kMidFreqLo     = 500.f,   kMidFreqHi     = 5000.f;
    static constexpr float kHighMidFreqLo = 1000.f,  kHighMidFreqHi = 10000.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalEqPanel)
};
