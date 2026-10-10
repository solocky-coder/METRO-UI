# Player ADSR ownership in METRO-UI

This note documents the current code ownership to avoid confusing the SF2-Player, SFZ-Player, and Multisampler envelope controls.

## SF2-Player (FluidSynth)

- Active mode: `uiMode == 2`.
- UI: `src/ui/Sf2WaveformLcd.cpp` / `.h`.
- UI behavior: draggable A/D/S/R nodes on the envelope graph; this is not a row of rotary knobs.
- UI-facing values: `processor.sfzPlayer` methods named `getSfzAttack/Decay/Sustain/Release` and `setSfzAttack/Decay/Sustain/Release`. These names are legacy and are misleading in the SF2 context.
- Synchronization: the graph calls `setJuceAdsr(...)` after edits. In the SF2 FluidSynth path, `SfzPlayer::applyFluidAdsrFromUi()` converts the values and writes `GEN_VOLENVATTACK`, `GEN_VOLENVDECAY`, `GEN_VOLENVSUSTAIN`, and `GEN_VOLENVRELEASE` to FluidSynth channels 2–15.

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

## Verification status

This document records the source-level call paths. It does not claim a live audio test or a successful build; those should be run in the project build environment.
