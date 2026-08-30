#include "MultisamplerZoneLcd.h"
#include "../DysektLookAndFeel.h"
#include "../UIHelpers.h"
#include "../../MidiLearnManager.h"
#include <cmath>

namespace
{
    // Same "hold Shift to fine-tune" convention SliceControlBar's drag cells
    // already use elsewhere in the plugin — kept identical here rather than
    // inventing a second modifier for the same idea.
    constexpr float kFineModeScale = 0.15f;

    // Identical sweep to SliceControlBar's kKnobStart/kKnobEnd (1.25π..2.75π,
    // i.e. a 270° sweep starting at 7:30) — kept as the same literal values
    // rather than a shared header so this component still doesn't take a
    // dependency on SliceControlBar (see this file's header doc comment).
    constexpr float kKnobStart = juce::MathConstants<float>::pi * 1.25f;
    constexpr float kKnobEnd   = juce::MathConstants<float>::pi * 2.75f;
}

MultisamplerZoneLcd::MultisamplerZoneLcd()
{
    setInterceptsMouseClicks (true, true);
}

// ── Display ──────────────────────────────────────────────────────────────

void MultisamplerZoneLcd::setZoneForDisplay (const SampleZone* zone, int displayIndex,
                                              bool isPreview, bool isAuditioning)
{
    if (zone == nullptr)
    {
        clearZone();
        return;
    }

    // Required snapshot copy — see this method's doc comment in the header.
    // Every field read here happens synchronously within this call; nothing
    // below retains `zone` itself.
    snapshot.valid          = true;
    snapshot.name           = zone->sampleFile.getFileName().isNotEmpty()
                                   ? zone->sampleFile.getFileNameWithoutExtension()
                                   : juce::String ("(no sample)");
    snapshot.displayIndex   = displayIndex;
    snapshot.lowKey         = zone->lowKey;
    snapshot.highKey        = zone->highKey;
    snapshot.rootKey        = zone->rootKey;
    snapshot.group          = zone->group;
    snapshot.outputBus      = zone->outputBus;
    snapshot.tuneCents      = zone->tuneCents;
    snapshot.pan            = zone->pan;
    snapshot.gainDb         = zone->gainDb;
    snapshot.attackSeconds  = zone->attackSeconds;
    snapshot.decaySeconds   = zone->decaySeconds;
    snapshot.sustainLevel   = zone->sustainLevel;
    snapshot.releaseSeconds = zone->releaseSeconds;
    snapshot.loopOn         = zone->loopMode != LoopMode::noLoop;
    snapshot.filterCutoffHz = zone->filterCutoffHz;
    snapshot.filterResonance = zone->filterResonance;
    snapshot.eq1Freq = zone->eq1Freq;
    snapshot.eq1Gain = zone->eq1Gain;
    snapshot.eq1Bw   = zone->eq1Bw;
    snapshot.eq2Freq = zone->eq2Freq;
    snapshot.eq2Gain = zone->eq2Gain;
    snapshot.eq2Bw   = zone->eq2Bw;
    snapshot.eq3Freq = zone->eq3Freq;
    snapshot.eq3Gain = zone->eq3Gain;
    snapshot.eq3Bw   = zone->eq3Bw;
    snapshot.showInMixer    = zone->showInMixer;
    snapshot.isPreview      = isPreview;
    snapshot.isAuditioning  = isAuditioning;

    repaint();
}

void MultisamplerZoneLcd::clearZone()
{
    snapshot = Snapshot{};
    repaint();
}

juce::String MultisamplerZoneLcd::getZoneTitleText() const
{
    if (! snapshot.valid)
        return {};

    return "ZONE " + juce::String (snapshot.displayIndex + 1) + "   " + snapshot.name;
}

void MultisamplerZoneLcd::setEditable (bool shouldEdit)
{
    if (editable == shouldEdit) return;
    editable = shouldEdit;
    repaint();
}

void MultisamplerZoneLcd::setOutputBusVisible (bool shouldShow)
{
    if (outputBusVisible == shouldShow) return;
    outputBusVisible = shouldShow;
    repaint();
}

// ── Field metadata ───────────────────────────────────────────────────────

juce::String MultisamplerZoneLcd::labelFor (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return "LO";
        case MultisamplerZoneField::highKey:     return "HI";
        case MultisamplerZoneField::rootKey:     return "ROOT";
        case MultisamplerZoneField::group:       return "GROUP";
        case MultisamplerZoneField::tune:        return "TUNE";
        case MultisamplerZoneField::pan:         return "PAN";
        case MultisamplerZoneField::gain:        return "GAIN";
        case MultisamplerZoneField::loopEnabled: return "LOOP";
        case MultisamplerZoneField::attack:      return "ATTACK";
        case MultisamplerZoneField::decay:       return "DECAY";
        case MultisamplerZoneField::sustain:     return "SUSTAIN";
        case MultisamplerZoneField::release:     return "RELEASE";
        case MultisamplerZoneField::cutoff:      return "FILTER";
        case MultisamplerZoneField::resonance:   return "RES";
        case MultisamplerZoneField::outputBus:   return "OUT";
        case MultisamplerZoneField::showInMixer: return "MIX";
        case MultisamplerZoneField::eq1Freq:     return "EQ1 F";
        case MultisamplerZoneField::eq1Gain:     return "EQ1 G";
        case MultisamplerZoneField::eq1Bw:       return "EQ1 BW";
        case MultisamplerZoneField::eq2Freq:     return "EQ2 F";
        case MultisamplerZoneField::eq2Gain:     return "EQ2 G";
        case MultisamplerZoneField::eq2Bw:       return "EQ2 BW";
        case MultisamplerZoneField::eq3Freq:     return "EQ3 F";
        case MultisamplerZoneField::eq3Gain:     return "EQ3 G";
        case MultisamplerZoneField::eq3Bw:       return "EQ3 BW";
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }
    return {};
}

float MultisamplerZoneLcd::getFieldValue (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return (float) snapshot.lowKey;
        case MultisamplerZoneField::highKey:     return (float) snapshot.highKey;
        case MultisamplerZoneField::rootKey:     return (float) snapshot.rootKey;
        case MultisamplerZoneField::group:       return (float) snapshot.group;
        case MultisamplerZoneField::tune:        return snapshot.tuneCents;
        case MultisamplerZoneField::pan:         return snapshot.pan;
        case MultisamplerZoneField::gain:        return snapshot.gainDb;
        case MultisamplerZoneField::loopEnabled: return snapshot.loopOn ? 1.0f : 0.0f;
        case MultisamplerZoneField::attack:      return snapshot.attackSeconds;
        case MultisamplerZoneField::decay:       return snapshot.decaySeconds;
        case MultisamplerZoneField::sustain:     return snapshot.sustainLevel;
        case MultisamplerZoneField::release:     return snapshot.releaseSeconds;
        case MultisamplerZoneField::cutoff:      return snapshot.filterCutoffHz;
        case MultisamplerZoneField::resonance:   return snapshot.filterResonance;
        case MultisamplerZoneField::outputBus:   return (float) snapshot.outputBus;
        case MultisamplerZoneField::showInMixer: return snapshot.showInMixer ? 1.0f : 0.0f;
        case MultisamplerZoneField::eq1Freq:     return snapshot.eq1Freq;
        case MultisamplerZoneField::eq1Gain:     return snapshot.eq1Gain;
        case MultisamplerZoneField::eq1Bw:       return snapshot.eq1Bw;
        case MultisamplerZoneField::eq2Freq:     return snapshot.eq2Freq;
        case MultisamplerZoneField::eq2Gain:     return snapshot.eq2Gain;
        case MultisamplerZoneField::eq2Bw:       return snapshot.eq2Bw;
        case MultisamplerZoneField::eq3Freq:     return snapshot.eq3Freq;
        case MultisamplerZoneField::eq3Gain:     return snapshot.eq3Gain;
        case MultisamplerZoneField::eq3Bw:       return snapshot.eq3Bw;
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }
    return 0.0f;
}

// Native value → 0-1 for the knob arc — see this method's doc comment in
// the header for where each range comes from.
float MultisamplerZoneLcd::normForField (MultisamplerZoneField field) const
{
    const float v = getFieldValue (field);
    switch (field)
    {
        case MultisamplerZoneField::lowKey:
        case MultisamplerZoneField::highKey:
        case MultisamplerZoneField::rootKey: return juce::jlimit (0.0f, 1.0f, v / 127.0f);
        case MultisamplerZoneField::group:   return juce::jlimit (0.0f, 1.0f, v / 32.0f);
        case MultisamplerZoneField::tune:    return juce::jlimit (0.0f, 1.0f, (v + 1200.0f) / 2400.0f);
        case MultisamplerZoneField::pan:     return juce::jlimit (0.0f, 1.0f, (v + 1.0f) * 0.5f);
        // Matches SliceControlBar::toNorm's FieldVolume mapping (-100..+24dB)
        // so a gain knob sweeps the same visual arc everywhere in the app.
        case MultisamplerZoneField::gain:    return juce::jlimit (0.0f, 1.0f, (v + 100.0f) / 124.0f);
        case MultisamplerZoneField::attack:  return juce::jlimit (0.0f, 1.0f, v / 2.0f);
        case MultisamplerZoneField::decay:   return juce::jlimit (0.0f, 1.0f, v / 5.0f);
        case MultisamplerZoneField::sustain: return juce::jlimit (0.0f, 1.0f, v);
        case MultisamplerZoneField::release: return juce::jlimit (0.0f, 1.0f, v / 5.0f);
        case MultisamplerZoneField::cutoff:
            return juce::jlimit (0.0f, 1.0f,
                (std::log2 (juce::jmax (20.0f, v)) - std::log2 (20.0f))
                    / (std::log2 (20000.0f) - std::log2 (20.0f)));
        case MultisamplerZoneField::resonance: return juce::jlimit (0.0f, 1.0f, v);
        case MultisamplerZoneField::loopEnabled: return v; // unused — LOOP draws as a toggle, not a knob
        // 0-15 range matches SampleZone::outputBus's own documented range
        // (0 = Main, 1-15 = Aux).
        case MultisamplerZoneField::outputBus: return juce::jlimit (0.0f, 1.0f, v / 15.0f);
        case MultisamplerZoneField::showInMixer: return v; // unused — MIX draws as a toggle, not a knob
        // Per-zone EQ — freq bands use the same log-frequency mapping as
        // `cutoff` above (each over its own documented range from
        // SampleZone.h); gain/bandwidth are linear over their documented
        // ranges, same idea as `tune`/`pan` above.
        case MultisamplerZoneField::eq1Freq:
            return juce::jlimit (0.0f, 1.0f,
                (std::log2 (juce::jmax (20.0f, v)) - std::log2 (20.0f))
                    / (std::log2 (1000.0f) - std::log2 (20.0f)));
        case MultisamplerZoneField::eq2Freq:
            return juce::jlimit (0.0f, 1.0f,
                (std::log2 (juce::jmax (100.0f, v)) - std::log2 (100.0f))
                    / (std::log2 (10000.0f) - std::log2 (100.0f)));
        case MultisamplerZoneField::eq3Freq:
            return juce::jlimit (0.0f, 1.0f,
                (std::log2 (juce::jmax (1000.0f, v)) - std::log2 (1000.0f))
                    / (std::log2 (20000.0f) - std::log2 (1000.0f)));
        case MultisamplerZoneField::eq1Gain:
        case MultisamplerZoneField::eq2Gain:
        case MultisamplerZoneField::eq3Gain:
            return juce::jlimit (0.0f, 1.0f, (v + 24.0f) / 48.0f);
        case MultisamplerZoneField::eq1Bw:
        case MultisamplerZoneField::eq2Bw:
        case MultisamplerZoneField::eq3Bw:
            return juce::jlimit (0.0f, 1.0f, (v - 0.1f) / 3.9f);
        case MultisamplerZoneField::kCount:
            break;   // sentinel, never a real field
    }
    return 0.5f;
}

// Algebraic inverse of normForField() above, case for case — kept as its own
// function (not a "pass a flag to invert" branch bolted onto normForField)
// since normForField needs an instance (getFieldValue()) and this doesn't;
// see its doc comment in the header for why that split matters for
// MultisamplerEditor::applyMidiLearnCc()'s absolute-CC case.
float MultisamplerZoneLcd::nativeFromNorm (MultisamplerZoneField field, float norm) noexcept
{
    norm = juce::jlimit (0.0f, 1.0f, norm);
    switch (field)
    {
        case MultisamplerZoneField::lowKey:
        case MultisamplerZoneField::highKey:
        case MultisamplerZoneField::rootKey:   return norm * 127.0f;
        case MultisamplerZoneField::group:     return norm * 32.0f;
        case MultisamplerZoneField::tune:      return norm * 2400.0f - 1200.0f;
        case MultisamplerZoneField::pan:       return norm * 2.0f - 1.0f;
        case MultisamplerZoneField::gain:      return norm * 124.0f - 100.0f;
        case MultisamplerZoneField::attack:    return norm * 2.0f;
        case MultisamplerZoneField::decay:     return norm * 5.0f;
        case MultisamplerZoneField::sustain:   return norm;
        case MultisamplerZoneField::release:   return norm * 5.0f;
        case MultisamplerZoneField::cutoff:
            return std::exp2 (std::log2 (20.0f) + norm * (std::log2 (20000.0f) - std::log2 (20.0f)));
        case MultisamplerZoneField::resonance: return norm;
        // Toggles — never reach this path; MultisamplerEditor::
        // applyMidiLearnCc() handles loopEnabled/showInMixer itself before
        // calling here. Returned as a harmless passthrough, not asserted,
        // in case a future caller queries them for some other reason.
        case MultisamplerZoneField::loopEnabled:   return norm;
        case MultisamplerZoneField::outputBus:     return norm * 15.0f;
        case MultisamplerZoneField::showInMixer:   return norm;
        case MultisamplerZoneField::eq1Freq:
            return std::exp2 (std::log2 (20.0f) + norm * (std::log2 (1000.0f) - std::log2 (20.0f)));
        case MultisamplerZoneField::eq2Freq:
            return std::exp2 (std::log2 (100.0f) + norm * (std::log2 (10000.0f) - std::log2 (100.0f)));
        case MultisamplerZoneField::eq3Freq:
            return std::exp2 (std::log2 (1000.0f) + norm * (std::log2 (20000.0f) - std::log2 (1000.0f)));
        case MultisamplerZoneField::eq1Gain:
        case MultisamplerZoneField::eq2Gain:
        case MultisamplerZoneField::eq3Gain:
            return norm * 48.0f - 24.0f;
        case MultisamplerZoneField::eq1Bw:
        case MultisamplerZoneField::eq2Bw:
        case MultisamplerZoneField::eq3Bw:
            return norm * 3.9f + 0.1f;
        case MultisamplerZoneField::kCount: break;   // sentinel, never a real field
    }
    return norm;
}

// Double-click reset targets — matches the model's own struct defaults in
// SampleZone.h so "reset" means "back to what a freshly added zone would
// have", not an arbitrary editor-chosen number.
float MultisamplerZoneLcd::defaultValueFor (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return 0.0f;
        case MultisamplerZoneField::highKey:     return 127.0f;
        case MultisamplerZoneField::rootKey:     return 60.0f;
        case MultisamplerZoneField::group:       return 0.0f;
        case MultisamplerZoneField::tune:        return 0.0f;
        case MultisamplerZoneField::pan:         return 0.0f;
        case MultisamplerZoneField::gain:        return 0.0f;
        case MultisamplerZoneField::loopEnabled: return 0.0f;
        case MultisamplerZoneField::attack:      return 0.005f;
        case MultisamplerZoneField::decay:       return 0.1f;
        case MultisamplerZoneField::sustain:     return 1.0f;
        case MultisamplerZoneField::release:     return 0.1f;
        case MultisamplerZoneField::cutoff:      return 20000.0f;
        case MultisamplerZoneField::resonance:   return 0.0f;
        case MultisamplerZoneField::outputBus:   return 0.0f;   // Main
        case MultisamplerZoneField::showInMixer: return 0.0f;   // hidden
        case MultisamplerZoneField::eq1Freq:     return 100.0f;
        case MultisamplerZoneField::eq1Gain:     return 0.0f;
        case MultisamplerZoneField::eq1Bw:       return 1.0f;
        case MultisamplerZoneField::eq2Freq:     return 1000.0f;
        case MultisamplerZoneField::eq2Gain:     return 0.0f;
        case MultisamplerZoneField::eq2Bw:       return 1.0f;
        case MultisamplerZoneField::eq3Freq:     return 8000.0f;
        case MultisamplerZoneField::eq3Gain:     return 0.0f;
        case MultisamplerZoneField::eq3Bw:       return 1.0f;
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }
    return 0.0f;
}

// Native units moved per pixel of vertical drag — same scale-per-field idea
// SliceControlBar::mouseDrag already used for the old SCB zone cells (see
// its ZonePan/ZoneVolume/etc. branches), carried over so drag feel doesn't
// change for anyone used to the old bar.
float MultisamplerZoneLcd::dragScaleFor (MultisamplerZoneField field, bool fineMode) noexcept
{
    float scale = 1.0f;
    switch (field)
    {
        case MultisamplerZoneField::lowKey:
        case MultisamplerZoneField::highKey:
        case MultisamplerZoneField::rootKey:     scale = 1.0f;   break;
        case MultisamplerZoneField::group:       scale = 1.0f;   break;
        case MultisamplerZoneField::tune:        scale = 1.0f;   break;
        case MultisamplerZoneField::pan:         scale = 0.01f;  break;
        case MultisamplerZoneField::gain:        scale = 0.5f;   break;
        case MultisamplerZoneField::attack:      scale = 0.01f;  break;
        case MultisamplerZoneField::decay:       scale = 0.05f;  break;
        case MultisamplerZoneField::sustain:     scale = 0.01f;  break;
        case MultisamplerZoneField::release:     scale = 0.01f;  break;
        case MultisamplerZoneField::cutoff:      scale = 50.0f;  break;
        case MultisamplerZoneField::resonance:   scale = 0.01f;  break;
        case MultisamplerZoneField::loopEnabled: scale = 0.0f;   break; // toggle, not a drag
        case MultisamplerZoneField::outputBus:   scale = 0.25f;  break; // ~4px per bus step
        case MultisamplerZoneField::showInMixer: scale = 0.0f;   break; // toggle, not a drag
        // Scaled to each field's own documented range (see SampleZone.h) —
        // roughly the same "full sweep over ~150-400px" feel as cutoff/pan
        // above, proportioned to how wide each range is.
        case MultisamplerZoneField::eq1Freq:     scale = 5.0f;   break; // 20..1000 Hz
        case MultisamplerZoneField::eq2Freq:     scale = 25.0f;  break; // 100..10000 Hz
        case MultisamplerZoneField::eq3Freq:     scale = 50.0f;  break; // 1000..20000 Hz
        case MultisamplerZoneField::eq1Gain:
        case MultisamplerZoneField::eq2Gain:
        case MultisamplerZoneField::eq3Gain:     scale = 0.2f;   break; // -24..+24 dB
        case MultisamplerZoneField::eq1Bw:
        case MultisamplerZoneField::eq2Bw:
        case MultisamplerZoneField::eq3Bw:       scale = 0.03f;  break; // 0.1..4.0 octaves
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }
    return fineMode ? scale * kFineModeScale : scale;
}

juce::String MultisamplerZoneLcd::formatFieldValue (MultisamplerZoneField field) const
{
    const auto note = [] (int n) { return UIHelpers::midiNoteToName (juce::jlimit (0, 127, n)); };
    switch (field)
    {
        case MultisamplerZoneField::lowKey:    return note (snapshot.lowKey);
        case MultisamplerZoneField::highKey:   return note (snapshot.highKey);
        case MultisamplerZoneField::rootKey:   return note (snapshot.rootKey);
        case MultisamplerZoneField::group:     return juce::String (snapshot.group);
        case MultisamplerZoneField::tune:
        {
            const int cents = juce::roundToInt (snapshot.tuneCents);
            return (cents >= 0 ? "+" : "") + juce::String (cents) + "ct";
        }
        case MultisamplerZoneField::pan:
            if (snapshot.pan == 0.0f) return "C";
            return (snapshot.pan < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (snapshot.pan) * 100.0f));
        case MultisamplerZoneField::gain:      return juce::String (snapshot.gainDb, 1) + "dB";
        case MultisamplerZoneField::loopEnabled: return snapshot.loopOn ? "ON" : "OFF";
        case MultisamplerZoneField::attack:    return juce::String (snapshot.attackSeconds, 3) + "s";
        case MultisamplerZoneField::decay:     return juce::String (snapshot.decaySeconds, 3) + "s";
        case MultisamplerZoneField::sustain:   return juce::String (juce::roundToInt (snapshot.sustainLevel * 100.0f)) + "%";
        case MultisamplerZoneField::release:   return juce::String (snapshot.releaseSeconds, 3) + "s";
        case MultisamplerZoneField::cutoff:
            return snapshot.filterCutoffHz >= 1000.0f
                       ? juce::String (snapshot.filterCutoffHz / 1000.0f, 1) + "kHz"
                       : juce::String (snapshot.filterCutoffHz, 0) + "Hz";
        case MultisamplerZoneField::resonance: return juce::String (snapshot.filterResonance, 2);
        case MultisamplerZoneField::outputBus:
            return snapshot.outputBus == 0 ? juce::String ("MAIN")
                                            : "AUX " + juce::String (snapshot.outputBus);
        case MultisamplerZoneField::showInMixer: return snapshot.showInMixer ? "SHOWN" : "HIDDEN";
        case MultisamplerZoneField::eq1Freq:
            return snapshot.eq1Freq >= 1000.0f
                       ? juce::String (snapshot.eq1Freq / 1000.0f, 1) + "kHz"
                       : juce::String (snapshot.eq1Freq, 0) + "Hz";
        case MultisamplerZoneField::eq2Freq:
            return snapshot.eq2Freq >= 1000.0f
                       ? juce::String (snapshot.eq2Freq / 1000.0f, 1) + "kHz"
                       : juce::String (snapshot.eq2Freq, 0) + "Hz";
        case MultisamplerZoneField::eq3Freq:
            return snapshot.eq3Freq >= 1000.0f
                       ? juce::String (snapshot.eq3Freq / 1000.0f, 1) + "kHz"
                       : juce::String (snapshot.eq3Freq, 0) + "Hz";
        case MultisamplerZoneField::eq1Gain:
            return (snapshot.eq1Gain >= 0.0f ? "+" : "") + juce::String (snapshot.eq1Gain, 1) + "dB";
        case MultisamplerZoneField::eq2Gain:
            return (snapshot.eq2Gain >= 0.0f ? "+" : "") + juce::String (snapshot.eq2Gain, 1) + "dB";
        case MultisamplerZoneField::eq3Gain:
            return (snapshot.eq3Gain >= 0.0f ? "+" : "") + juce::String (snapshot.eq3Gain, 1) + "dB";
        case MultisamplerZoneField::eq1Bw: return juce::String (snapshot.eq1Bw, 2);
        case MultisamplerZoneField::eq2Bw: return juce::String (snapshot.eq2Bw, 2);
        case MultisamplerZoneField::eq3Bw: return juce::String (snapshot.eq3Bw, 2);
        case MultisamplerZoneField::kCount: break;   // sentinel, never a real field
    }
    return {};
}

// ── Layout / paint ───────────────────────────────────────────────────────

void MultisamplerZoneLcd::resized()
{
    // Cell rects are (re)built in paint() against the current bounds, same
    // pattern SliceControlBar uses for its own ParamCell/SfzZoneCell hit
    // areas — resized() only needs to trigger a repaint.
    repaint();
}

void MultisamplerZoneLcd::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    auto bounds = getLocalBounds();

    g.setColour (theme.darkBar.darker (0.15f));
    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);
    g.setColour (theme.separator);
    g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 4.0f, 1.0f);

    cells.clear();

    if (! snapshot.valid)
    {
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (12.0f * uiScale, false));
        g.drawText ("NO ZONE SELECTED", bounds, juce::Justification::centred);
        return;
    }

    auto content = bounds.reduced (8, 4);

    // Title row (name/index, PREVIEW/AUDITIONING badge) no longer drawn
    // here — it moved out to MultisamplerEditor's header toolbar
    // (zoneTagLabel/zoneBadgeLabel, kept in sync from getZoneTitleText()/
    // isShowingPreview()/isShowingAuditioning() via refreshZoneLcdDisplay()).
    // All of `content` now goes straight to the knob grid.

    // Three rows of knob cells — row 1 has 7 fields (mapping + tune/pan/gain),
    // row 2 has 9 (envelope/filter/routing, including the LOOP and MIX flat
    // toggles) or 8 when OUT is excluded (see outputBusVisible below), row 3
    // has 9 (the EQ1/EQ2/EQ3 band controls) — matching
    // SliceControlBar's own knob-row layouts (drawKnobCell in a straight
    // horizontal run) instead of the old cramped 4/4/6 text-cell split.
    const int rowH = content.getHeight() / 3;

    // Takes a vector rather than an initializer_list so row 2 below can be
    // built conditionally (OUT dropped entirely when !outputBusVisible,
    // rather than left in the list and merely skipped — the row's cellW
    // divides by however many fields actually get passed in, so dropping
    // OUT here is also what makes the remaining fields reflow to fill the
    // gap instead of leaving a blank cell where it used to sit).
    auto layoutRow = [&] (juce::Rectangle<int> row, const std::vector<MultisamplerZoneField>& fields)
    {
        const int n = (int) fields.size();
        const int cellW = row.getWidth() / juce::jmax (1, n);
        int x = row.getX();
        for (auto f : fields)
        {
            juce::Rectangle<int> cellBounds (x, row.getY(), cellW, row.getHeight());
            cells.push_back ({ cellBounds, f });
            drawCell (g, cellBounds, f);
            x += cellW;
        }
    };

    layoutRow (content.removeFromTop (rowH),
               { MultisamplerZoneField::lowKey, MultisamplerZoneField::highKey,
                 MultisamplerZoneField::rootKey,
                 MultisamplerZoneField::tune, MultisamplerZoneField::pan,
                 MultisamplerZoneField::gain });

    // OUT is only meaningful when something downstream can actually honour
    // an AUX 1–15 choice — see setOutputBusVisible()'s doc comment in the
    // header for why the standalone build can't. Everything else in this
    // row is unconditional.
    std::vector<MultisamplerZoneField> row2 {
        MultisamplerZoneField::loopEnabled, MultisamplerZoneField::attack,
        MultisamplerZoneField::decay, MultisamplerZoneField::sustain,
        MultisamplerZoneField::release, MultisamplerZoneField::cutoff,
        MultisamplerZoneField::resonance
    };
    if (outputBusVisible)
        row2.push_back (MultisamplerZoneField::outputBus);
    row2.push_back (MultisamplerZoneField::showInMixer);
    layoutRow (content.removeFromTop (rowH), row2);

    layoutRow (content,
               { MultisamplerZoneField::eq1Freq, MultisamplerZoneField::eq1Gain,
                 MultisamplerZoneField::eq1Bw, MultisamplerZoneField::eq2Freq,
                 MultisamplerZoneField::eq2Gain, MultisamplerZoneField::eq2Bw,
                 MultisamplerZoneField::eq3Freq, MultisamplerZoneField::eq3Gain,
                 MultisamplerZoneField::eq3Bw });
}

void MultisamplerZoneLcd::drawCell (juce::Graphics& g, juce::Rectangle<int> bounds, MultisamplerZoneField field)
{
    // cells.back() at this point is exactly this cell (paint()'s layoutRow()
    // pushes it just before calling drawCell()) — used to resolve this
    // cell's own index for the hover/drag comparisons below.
    const int idx = (int) cells.size() - 1;

    if (field == MultisamplerZoneField::loopEnabled)
        drawLoopToggleCell (g, bounds, idx);
    else if (field == MultisamplerZoneField::showInMixer)
        drawMixerToggleCell (g, bounds, idx);
    else
        drawKnobField (g, bounds, field, idx);

    // ── MIDI Learn CC label overlay ──────────────────────────────────────
    // Drawn one level up, after the dispatch above, rather than threaded
    // into drawKnobField/drawLoopToggleCell/drawMixerToggleCell individually
    // — every field type gets it uniformly from one place. midiLearn is
    // null until MultisamplerEditor calls setMidiLearnManager() (see that
    // method's doc comment); until then this silently draws nothing.
    if (midiLearn != nullptr)
    {
        const int slot   = midiLearnSlotFor (field);
        const bool mapped = midiLearn->isMapped (slot);
        const bool armed  = midiLearn->getArmedSlot() == slot;
        if (mapped || armed)
        {
            g.setFont (DysektLookAndFeel::makeMonoFont (8.0f * uiScale));
            g.setColour (armed ? getTheme().accent
                                : getTheme().foreground.withAlpha (0.6f));
            g.drawText (armed ? juce::String ("LEARN") : midiLearn->getLabelText (slot),
                        bounds.removeFromTop (juce::roundToInt (10.0f * uiScale))
                              .reduced (juce::roundToInt (2.0f * uiScale), 0),
                        juce::Justification::topRight);
        }
    }
}

// =============================================================================
// drawKnobArc — small rotary arc. Same shape as SliceControlBar::drawKnob:
// thin arc track, single accent fill arc, plain indicator line, flat centre
// dot — no gradient, no rounded rect, no glow. Deliberately doesn't take
// SliceControlBar::drawKnob's locked/armed/mapped/tintOverride params —
// none of those concepts exist for a multisampler zone field.
// =============================================================================
void MultisamplerZoneLcd::drawKnobArc (juce::Graphics& g, int cx, int cy, int r,
                                        float normVal, bool hovered, bool dragging) const
{
    const auto& theme = getTheme();

    // Hover ring: same radius offset (r+3.5) and stroke (1.2f) as
    // SliceControlBar::drawKnob's hover ring — was r+3.0, close enough to
    // pass at a glance but visibly a different ring size next to an actual
    // SCB knob at the same radius.
    if (hovered && ! dragging)
    {
        const float fr2 = (float) r + 3.5f;
        g.setColour (theme.accent.withAlpha (0.18f));
        g.drawEllipse ((float) cx - fr2, (float) cy - fr2, fr2 * 2.0f, fr2 * 2.0f, 1.2f);
    }

    const float angle = kKnobStart + normVal * (kKnobEnd - kKnobStart);
    const float fcx = (float) cx, fcy = (float) cy, fr = (float) r;

    juce::Path track;
    track.addCentredArc (fcx, fcy, fr, fr, 0.0f, kKnobStart, kKnobEnd, true);
    g.setColour (theme.darkBar.brighter (0.22f));
    g.strokePath (track, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Resting/editable alpha (0.55) matches SliceControlBar::drawKnob's own
    // default arcCol for an unarmed/unmapped/unlocked knob
    // (base.withAlpha(0.55f)) — was 0.7f, visibly brighter than the same
    // knob drawn on the SCB even at identical radius and normVal. Disabled
    // (not editable) keeps the same proportional dimming it had before
    // relative to the corrected resting value.
    const juce::Colour arcCol = dragging ? theme.accent
                                          : editable ? theme.accent.withAlpha (0.55f)
                                                     : theme.accent.withAlpha (0.32f);

    juce::Path arc;
    arc.addCentredArc (fcx, fcy, fr, fr, 0.0f, kKnobStart, angle, true);
    g.setColour (arcCol);
    // Stroke width matches SliceControlBar::drawKnob's arc (2.2f) — was 2.0f.
    g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float lineAngle = angle - juce::MathConstants<float>::halfPi;
    const float lineR = fr - 2.5f;
    g.setColour (arcCol.brighter (0.15f));
    g.drawLine (fcx, fcy, fcx + lineR * std::cos (lineAngle), fcy + lineR * std::sin (lineAngle), 1.5f);

    // Centre dot alpha matches SliceControlBar::drawKnob's default (unlocked)
    // dot (foreground.withAlpha(0.25f)) — was 0.35f for the editable case.
    g.setColour (theme.foreground.withAlpha (editable ? 0.25f : 0.15f));
    g.fillEllipse (fcx - 2.0f, fcy - 2.0f, 4.0f, 4.0f);
}

// =============================================================================
// drawKnobField — rotary knob cell: arc on the left, LABEL above VALUE to its
// right. Same visual arrangement as SliceControlBar::drawKnobCell, minus the
// MIDI Learn pulse/lock-icon/pickup-chase machinery that doesn't apply here.
// =============================================================================
void MultisamplerZoneLcd::drawKnobField (juce::Graphics& g, juce::Rectangle<int> bounds,
                                          MultisamplerZoneField field, int cellIdx)
{
    const auto& theme = getTheme();
    auto r = bounds.reduced (2, 1);

    const bool hoveredNow  = editable && (cellIdx == hoveredCellIdx);
    const bool draggingNow = haveActiveDrag && activeField == field;

    // Knob radius matches SliceControlBar::psKnobR exactly (kKnobR * scale)
    // instead of a per-cell dynamic size — this LCD's knobs sat next to the
    // SCB's own knobs often enough (same instrument, same session) that a
    // different radius read as an inconsistency rather than a deliberate
    // layout choice. SCB derives its scale from getHeight()/72 (paintSf);
    // this component's uiScale is set from the same global UI-scale value
    // PluginEditor passes everywhere else (see MultisamplerEditor::
    // setUiScale), so the two stay numerically equal without this
    // component needing to know anything about the SCB's own height.
    const int knobR  = juce::roundToInt ((float) kKnobR * uiScale);
    const int knobCX = r.getX() + knobR + juce::roundToInt (4.0f * uiScale);
    const int knobCY = r.getY() + r.getHeight() / 2;

    drawKnobArc (g, knobCX, knobCY, knobR, normForField (field), hoveredNow, draggingNow);

    const int textX = knobCX + knobR + juce::roundToInt (8.0f * uiScale);
    const int textW = juce::jmax (0, r.getRight() - textX);

    // Label/value alphas match SliceControlBar::drawKnobCell's own default
    // (unlocked, non-ADSR) cell: label foreground.withAlpha(0.42f), value
    // foreground.withAlpha(0.38f) — this component previously used 0.55f/
    // full-opacity for its "editable" state, which is actually brighter
    // than SCB's ordinary knob text and only matched SCB's LOCKED-highlight
    // brightness by coincidence. Disabled (not editable) keeps the same
    // proportional dimming it had before relative to the corrected values.
    g.setColour (editable ? theme.foreground.withAlpha (0.42f) : theme.foreground.withAlpha (0.24f));
    g.setFont (DysektLookAndFeel::makeFont (9.5f * uiScale, false));
    g.drawText (labelFor (field), textX, r.getY(), textW, r.getHeight() / 2, juce::Justification::centredLeft);

    g.setColour (editable ? theme.foreground.withAlpha (0.38f) : theme.foreground.withAlpha (0.22f));
    g.setFont (DysektLookAndFeel::makeMonoFont (13.0f * uiScale, true));
    g.drawText (formatFieldValue (field), textX, r.getY() + r.getHeight() / 2,
                textW, r.getHeight() - r.getHeight() / 2, juce::Justification::centredLeft);
}

// =============================================================================
// drawLoopToggleCell — LOOP is boolean, so it draws as a flat ON/OFF badge
// rather than a knob (a 0..1 arc reads as a fader, not a toggle, for a
// binary field) — same treatment this component used for LOOP before.
// =============================================================================
void MultisamplerZoneLcd::drawLoopToggleCell (juce::Graphics& g, juce::Rectangle<int> bounds, int cellIdx)
{
    const auto& theme = getTheme();
    auto r = bounds.reduced (2, 1);

    const bool hoveredNow  = editable && (cellIdx == hoveredCellIdx);
    const bool draggingNow = haveActiveDrag && activeField == MultisamplerZoneField::loopEnabled;

    if (draggingNow || hoveredNow)
    {
        g.setColour (draggingNow ? theme.accent.withAlpha (0.25f) : theme.accent.withAlpha (0.12f));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    }

    g.setColour (editable ? theme.foreground.withAlpha (0.55f) : theme.foreground.withAlpha (0.32f));
    g.setFont (DysektLookAndFeel::makeFont (9.5f * uiScale, false));
    auto labelRow = r.removeFromTop (r.getHeight() / 2);
    g.drawText (labelFor (MultisamplerZoneField::loopEnabled), labelRow, juce::Justification::centredLeft);

    g.setColour (snapshot.loopOn ? theme.accent : (editable ? theme.foreground : theme.foreground.withAlpha (0.6f)));
    g.setFont (DysektLookAndFeel::makeMonoFont (13.0f * uiScale, true));
    g.drawText (formatFieldValue (MultisamplerZoneField::loopEnabled), r, juce::Justification::centredLeft);
}

// =============================================================================
// drawMixerToggleCell — MIX is boolean, same flat badge treatment as LOOP
// above. This is the zone-level equivalent of SliceControlBar's own
// drawMixerToggleCell (show-in-MixerPanel pin/hide toggle for a Slicer/
// SFZ-PLAYER slice) — see SampleZone::showInMixer's doc comment for how the
// underlying flag reaches an actual MixerPanel row.
// =============================================================================
void MultisamplerZoneLcd::drawMixerToggleCell (juce::Graphics& g, juce::Rectangle<int> bounds, int cellIdx)
{
    const auto& theme = getTheme();
    auto r = bounds.reduced (2, 1);

    const bool hoveredNow  = editable && (cellIdx == hoveredCellIdx);
    const bool draggingNow = haveActiveDrag && activeField == MultisamplerZoneField::showInMixer;

    if (draggingNow || hoveredNow)
    {
        g.setColour (draggingNow ? theme.accent.withAlpha (0.25f) : theme.accent.withAlpha (0.12f));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    }

    g.setColour (editable ? theme.foreground.withAlpha (0.55f) : theme.foreground.withAlpha (0.32f));
    g.setFont (DysektLookAndFeel::makeFont (9.5f * uiScale, false));
    auto labelRow = r.removeFromTop (r.getHeight() / 2);
    g.drawText (labelFor (MultisamplerZoneField::showInMixer), labelRow, juce::Justification::centredLeft);

    g.setColour (snapshot.showInMixer ? theme.accent : (editable ? theme.foreground : theme.foreground.withAlpha (0.6f)));
    g.setFont (DysektLookAndFeel::makeMonoFont (13.0f * uiScale, true));
    g.drawText (formatFieldValue (MultisamplerZoneField::showInMixer), r, juce::Justification::centredLeft);
}

// ── Mouse / editing ──────────────────────────────────────────────────────

void MultisamplerZoneLcd::mouseMove (const juce::MouseEvent& e)
{
    int newHover = -1;
    for (int i = 0; i < (int) cells.size(); ++i)
        if (cells[(size_t) i].bounds.contains (e.getPosition())) { newHover = i; break; }

    if (newHover != hoveredCellIdx)
    {
        hoveredCellIdx = newHover;
        repaint();
    }
}

void MultisamplerZoneLcd::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCellIdx != -1) { hoveredCellIdx = -1; repaint(); }
}

void MultisamplerZoneLcd::mouseDown (const juce::MouseEvent& e)
{
    if (! editable || ! snapshot.valid) return;

    for (const auto& cell : cells)
    {
        if (! cell.bounds.contains (e.getPosition())) continue;

        if (e.mods.isPopupMenu())
        {
            // Right-click anywhere on a field cell arms/clears MIDI Learn —
            // same right-click-to-learn convention SliceControlBar's own
            // knob cells use. No drag/toggle gesture to worry about here,
            // unlike the left-click branches below: a right-click never
            // starts a drag or flips a toggle, so this returns before any
            // of that logic runs, whichever field type was hit.
            if (onFieldLearnMenuRequested)
                onFieldLearnMenuRequested (cell.field, e.getScreenPosition());
            return;
        }

        if (cell.field == MultisamplerZoneField::loopEnabled)
        {
            // Click toggles immediately — no drag gesture for a boolean.
            applyDrag (cell.field, snapshot.loopOn ? 0.0f : 1.0f, /*commit=*/true);
            return;
        }

        if (cell.field == MultisamplerZoneField::showInMixer)
        {
            // Click toggles immediately — no drag gesture for a boolean.
            applyDrag (cell.field, snapshot.showInMixer ? 0.0f : 1.0f, /*commit=*/true);
            return;
        }

        haveActiveDrag = true;
        dragMoved      = false;
        activeField    = cell.field;
        dragStartValue = getFieldValue (cell.field);
        dragStartY     = e.getScreenY();
        return;
    }
}

void MultisamplerZoneLcd::mouseDrag (const juce::MouseEvent& e)
{
    if (! haveActiveDrag || ! editable) return;

    const int deltaPixels = dragStartY - e.getScreenY();   // up = increase, matches SliceControlBar convention
    if (deltaPixels != 0) dragMoved = true;

    const bool fineMode = e.mods.isShiftDown();
    const float scale = dragScaleFor (activeField, fineMode);
    const float newValue = dragStartValue + (float) deltaPixels * scale;

    applyDrag (activeField, newValue, /*commit=*/false);
}

void MultisamplerZoneLcd::mouseUp (const juce::MouseEvent&)
{
    if (! haveActiveDrag) return;
    haveActiveDrag = false;

    // Commit once at gesture end — matches plan §6: dirty/undo/history
    // should land once per gesture, not once per mouse pixel. Only commit
    // if the value actually moved; a plain click with no drag shouldn't
    // register as an edit.
    if (dragMoved)
        applyDrag (activeField, getFieldValue (activeField), /*commit=*/true);

    repaint();
}

void MultisamplerZoneLcd::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! editable || ! snapshot.valid) return;

    for (const auto& cell : cells)
    {
        if (! cell.bounds.contains (e.getPosition())) continue;
        applyDrag (cell.field, defaultValueFor (cell.field), /*commit=*/true);
        return;
    }
}

void MultisamplerZoneLcd::applyDrag (MultisamplerZoneField field, float rawValue, bool commit)
{
    // Update the local snapshot immediately so the LCD repaints live during
    // a drag, exactly as plan §6 asks — but this is a *display* update
    // only. MultisamplerEditor::applyZoneFieldEdit is still the sole place
    // that clamps and writes the authoritative model value; if this
    // component's clamp-free display value differs from what gets clamped
    // upstream, the next setZoneForDisplay() call (fired via
    // onZoneSelectionOrEditChanged after the model write) overwrites it
    // with the true value, so there's no lasting drift.
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      snapshot.lowKey  = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::highKey:     snapshot.highKey = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::rootKey:     snapshot.rootKey = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::group:       snapshot.group   = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::tune:        snapshot.tuneCents = rawValue; break;
        case MultisamplerZoneField::pan:         snapshot.pan = rawValue; break;
        case MultisamplerZoneField::gain:        snapshot.gainDb = rawValue; break;
        case MultisamplerZoneField::loopEnabled: snapshot.loopOn = rawValue > 0.5f; break;
        case MultisamplerZoneField::attack:      snapshot.attackSeconds = rawValue; break;
        case MultisamplerZoneField::decay:       snapshot.decaySeconds = rawValue; break;
        case MultisamplerZoneField::sustain:     snapshot.sustainLevel = rawValue; break;
        case MultisamplerZoneField::release:     snapshot.releaseSeconds = rawValue; break;
        case MultisamplerZoneField::cutoff:      snapshot.filterCutoffHz = rawValue; break;
        case MultisamplerZoneField::resonance:   snapshot.filterResonance = rawValue; break;
        case MultisamplerZoneField::outputBus:   snapshot.outputBus = juce::jlimit (0, 15, juce::roundToInt (rawValue)); break;
        case MultisamplerZoneField::showInMixer: snapshot.showInMixer = rawValue > 0.5f; break;
        case MultisamplerZoneField::eq1Freq:     snapshot.eq1Freq = rawValue; break;
        case MultisamplerZoneField::eq1Gain:     snapshot.eq1Gain = rawValue; break;
        case MultisamplerZoneField::eq1Bw:       snapshot.eq1Bw   = rawValue; break;
        case MultisamplerZoneField::eq2Freq:     snapshot.eq2Freq = rawValue; break;
        case MultisamplerZoneField::eq2Gain:     snapshot.eq2Gain = rawValue; break;
        case MultisamplerZoneField::eq2Bw:       snapshot.eq2Bw   = rawValue; break;
        case MultisamplerZoneField::eq3Freq:     snapshot.eq3Freq = rawValue; break;
        case MultisamplerZoneField::eq3Gain:     snapshot.eq3Gain = rawValue; break;
        case MultisamplerZoneField::eq3Bw:       snapshot.eq3Bw   = rawValue; break;
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }

    repaint();
    if (onFieldEdited) onFieldEdited (field, rawValue, commit);
}
