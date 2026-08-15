# Multisampler — Phase 1 implementation

This is the "Recommended First Development Slice" from
`METRO-UI_MULTISAMPLER_IMPLEMENTATION.md` (§14), extended to match §4/§5's
full Phase 1 scope ("Model and reliable SFZ round-trip").

## What's here

| File | Purpose |
|---|---|
| `SampleZone.h` | Plain-data per-region struct. `LoopMode` mirrors SFZ's four `loop_mode` values exactly. |
| `MultisamplerInstrument.h/.cpp` | The native model: zone CRUD, overlap/missing-sample queries for the mapping editor, `validate()`, and bundle-relink helpers. |
| `SfzImporter.h/.cpp` | Standalone tokenizing SFZ parser — **not** built on `SfzModulePanel`'s existing line-oriented scanner, per the plan's explicit instruction. Handles `<global>`/`<group>`/`<region>` inheritance, quoted and unquoted (space-containing) sample paths, multiple opcodes per line, and comments. Unsupported opcodes are preserved on `SampleZone::extraOpcodes` instead of being dropped. |
| `SfzExporter.h/.cpp` | Renders the model back to SFZ for sfizz playback and for the "Export SFZ" bundle action. Emits one fully-resolved `<region>` per zone (see the header comment for why it doesn't reconstruct `<global>`/`<group>` inheritance). |
| `InstrumentSerializer.h/.cpp` | `instrument.json` (de)serialization — the *lossless* round trip for `.metrokit` bundles, independent of anything SFZ can't express (e.g. `masterGainDb`, `maxVoices`). |
| `tests/MultisamplerRoundTripTests.cpp` | `juce::UnitTest` coverage for tokenization/inheritance, the SFZ round trip, the JSON round trip, and `validate()`. |

## What's deliberately not here yet

Everything UI-facing (`ZoneMapView`, `ZoneInspector`, drag-and-drop
auto-mapping, the debounced sfizz sync bridge) is Phase 2/3 per the plan and
depends on this model existing first. The interactive HTML mockup delivered
earlier in this conversation shows the intended UI; wiring it to real JUCE
components against this model is the next slice.

Also out of scope for Phase 1, per the plan: `.metrokit` folder/zip packaging
and Collect Samples/Relink orchestration (Phase 4), `#include` directives,
keyswitches, curves, and the `<control>`/`<effect>`/`<master>` headers
(explicitly deferred by §5).

## Wiring this into the existing project

1. Add every `.cpp` under `src/audio/multisampler/` (including
   `tests/MultisamplerRoundTripTests.cpp` if your test target is separate
   from the plugin target) to the build — this drop has no CMakeLists.txt of
   its own since none was included in the source archive; add the files to
   whatever target already builds `src/audio/SfzPlayer.cpp`.
2. These files depend only on `juce_core` (plus `juce::File`/`juce::Uuid`,
   both in `juce_core`) — no `juce_audio_*` or `juce_gui_*` needed, so they
   can be exercised by a headless test target if you have one.
3. Nothing here talks to `SfzPlayer`/sfizz directly. The plan's §5 "Playback
   synchronization" step (debounce → export to cache dir → background-load
   replacement synth → swap at a block boundary) is the next piece to add,
   most naturally as a small `MultisamplerEngineBridge` that owns a
   `MultisamplerInstrument`, calls `SfzExporter::exportToFile()` on a
   background thread after edits settle, and hands the result to
   `SfzPlayer::loadFile()` on the existing async path.
4. I wasn't able to compile this against your actual JUCE version/toolchain
   in this environment (no JUCE checkout or build files were in the
   uploaded archive, and this sandbox's network access doesn't reach
   JUCE/sfizz sources) — please build it once against your project as a
   sanity check, in particular `juce::Uuid::null()` (JUCE 6.1+) and
   `juce::RangedDirectoryIterator` (JUCE 7+), both used here on the
   assumption that your JUCE is recent (your `MetroTypography.h` already
   uses `juce::FontOptions`, which implies JUCE 7+).
