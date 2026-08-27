// Snippet from SfzExporter.cpp:
if (z.filterCutoffHz < 20000.0f)
    out << "cutoff=" << juce::String (z.filterCutoffHz, 1) << "\n";
if (z.filterResonance > 0.0f)
    out << "resonance=" << juce::String (z.filterResonance * 40.0f, 2) << "\n";

// Per-Zone EQ (SFZ Standard opcodes understood natively by sfizz)
if (z.eqEnabled)
{
    if (z.eq1Gain != 0.0f || z.eq1Freq != 80.0f)
        out << "eq1_freq=" << juce::String (z.eq1Freq, 1)
            << " eq1_gain=" << juce::String (z.eq1Gain, 2)
            << " eq1_bw=" << juce::String (z.eq1Bw, 2) << "\n";

    if (z.eq2Gain != 0.0f || z.eq2Freq != 1000.0f)
        out << "eq2_freq=" << juce::String (z.eq2Freq, 1)
            << " eq2_gain=" << juce::String (z.eq2Gain, 2)
            << " eq2_bw=" << juce::String (z.eq2Bw, 2) << "\n";

    if (z.eq3Gain != 0.0f || z.eq3Freq != 8000.0f)
        out << "eq3_freq=" << juce::String (z.eq3Freq, 1)
            << " eq3_gain=" << juce::String (z.eq3Gain, 2)
            << " eq3_bw=" << juce::String (z.eq3Bw, 2) << "\n";
}