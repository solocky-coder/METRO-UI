# Player ADSR ownership in METRO-UI

This note documents the current code ownership to avoid confusing the SF2-Player, SFZ-Player, and Multisampler envelope controls.

## SF2-Player (FluidSynth)

- Active mode: `uiMode == 2`.
- UI: `src/ui/Sf2WaveformLcd.cpp` / `.h`.
- UI behavior: draggable A/D/S/R nodes on the envelope graph; this is not a row of rotary knobs.
- UI-facing values: `processor.sfzPlayer` methods named `getSfzAttack/Decay/Sustain/Release` and `setSfzAttack/Decay/Sustain/Release`. These names are legacy and are misleading in the SF2 context.
- Synchronization: the graph calls `setJuceAdsr(...)` after edits. In the SF2 FluidSynth path, `SfzPlayer::applyFluidAdsrFromUi()` converts values to native timecents/centibels and uses `fluid_synth_set_gen2(..., absolute=1, normalized=0)` for `GEN_VOLENVATTACK`, `GEN_VOLENVDECAY`, `GEN_VOLENVSUSTAIN`, and `GEN_VOLENVRELEASE` on channels 2–15. Do not replace this with `fluid_synth_set_gen()`: that API applies an additive offset, not an absolute override.

## SFZ-Player and legacy dropdown panel

- `SfzPlayerDropdownPanel` is a separate component from `Sf2WaveformLcd`.
- Its legacy ADSR rotary knob code calls `processor.sfzPlayer2.setSfzAttack/Decay/Sustain/Release`, not `processor.sfzPlayer`.
- `PluginEditor.cpp` currently comments that `sfzPlayerDropdown` is not shown in the live SFZ-Player mode. Check the actual visibility call sites before assuming this panel's controls are user-accessible.
- Do not use the existence of `adsrAtkZone`, `adsrDecZone`, `adsrSusZone`, or `adsrRelZone` in this panel as evidence of SF2 rotary knobs.

## Multisampler

The Multisampler has its own zone-level envelope data and edit callbacks. Its envelope is per selected sample zone and should not be conflated with the SF2-Player's FluidSynth volume envelope or the legacy SFZ-player-level ADSR values.

## Naming and maintenance guidance

1. Treat `sfzPlayer` and `sfzPlayer2` as distinct engine instances until a deliberate, fully traced rename is performed.
2. Avoid a broad mechanical rename of the `setSfz...` methods: they are used by more than one playback path. First split the SF2 and SFZ APIs, migrate all call sites, and add tests.
3. If the legacy dropdown panel is retired, remove it only after confirming its non-ADSR helpers (preset/file loading, SF2 channel FX, and zone utilities) have no remaining callers.
4. Verify envelope behavior with a built plugin: test each ADSR stage on an SF2 preset, then separately test SFZ and Multisampler envelopes.

## ADSR semantics and verification checklist

- The SF2 graph is intended to express absolute attack/decay/sustain/release values. FluidSynth's `fluid_synth_set_gen()` applies offsets, so using it with converted UI values changes the preset values additively and can produce unexpectedly long/short envelopes. The SF2 path now uses `fluid_synth_set_gen2(..., 1, 0)` to request absolute native-unit generator values.
- Sustain is a level percentage in the UI but attenuation in centibels in SF2. 100% maps to 0 cB; 0% maps to the 1000 cB (-100 dB) floor.
- Reapplication is required after SF2 load and program/preset changes because those operations reset generator state. It also runs when the UI ADSR parameters change.
- Runtime verification still requires a build with the target FluidSynth version and audio tests: check attack/decay/sustain/release independently, switch between presets with substantially different native envelopes, confirm the UI's custom envelope remains consistent, test 0% and 100% sustain, and verify releasing one note does not fade another held note.

This documents the implementation and the verification procedure; it does not claim a successful build or live audio test.
