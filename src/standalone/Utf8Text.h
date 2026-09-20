#pragma once

#include <juce_core/juce_core.h>

// juce::String(const char*) requires strictly-ASCII input — see the doc
// comment on that constructor in juce_String.h: "The string passed-in must
// not contain any characters with a value above 127". /utf-8 (set for both
// CMake targets) makes MSVC compile a literal like "AOO \u2022 Direct..."
// into the correct raw UTF-8 bytes, but that compiler flag only controls
// how the literal is *encoded in the binary* — it says nothing about how
// juce::String's implicit const-char* constructor *decodes* those bytes at
// runtime. That constructor does a raw byte-for-byte copy (effectively
// Windows-1252), not a UTF-8 decode. So a plain "AOO \u2022 Direct..." literal
// still gets mangled on screen into "AOO \u00e2\u20ac\u00a2 Direct..." even
// though the bytes in the binary were always correct UTF-8 — the corruption
// happens at construction, not compilation.
//
// Route any string literal containing non-ASCII characters through this
// helper instead of handing it to juce::String directly. It's the same
// bytes, just explicitly tagged as UTF-8 so JUCE decodes them properly:
//
//     label.setText (utf8 ("Direct USB \u2014 listening"), juce::dontSendNotification);
//
// instead of:
//
//     label.setText ("Direct USB \u2014 listening", juce::dontSendNotification); // mojibake
inline juce::String utf8 (const char* utf8Text)
{
    return juce::String (juce::CharPointer_UTF8 (utf8Text));
}
