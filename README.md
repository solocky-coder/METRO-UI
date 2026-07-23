# DYSEKT-SF

**A creative sampler, slicer, multisample mapper, and SoundFont player.**

DYSEKT-SF combines three focused sound workflows in one instrument: slice and perform audio recordings, play or author SFZ instruments, and browse SoundFont libraries. It is available as a VST3 instrument and as a standalone application.

> Start with the sound source—not a complicated setup. Chop a recording, load an SFZ, or open an SF2 and play.

## Features

- **Slicer** — turn audio into a playable, slice-based instrument
- **SFZ Player** — load, inspect, play, and author SFZ sample mappings
- **SF2 Player** — browse SoundFont presets and perform them over MIDI
- **Mixer** — balance rows and master output with gain, pan, filtering, mute groups, and routing
- **Global EQ** — five-band output EQ with spectrum display
- Familiar sound-shaping controls including pitch, tuning, level, pan, envelope, filtering, looping, output selection, and MIDI performance settings

## Workspaces

### Slicer

Load an audio file and divide it into playable slices. The waveform shows each slice as a coloured region so loops, breaks, hits, and textures can quickly become expressive performance kits.

1. Choose **SLICER**.
2. Load an audio file.
3. Select a slice in the waveform.
4. Shape the selected sound with the lower control strip.

The selected-slice panel shows the slice name, assigned note, start/end information, and primary sound values.

### SFZ Player

Load an SFZ instrument and view its sample mapping across the keyboard. The mapping display uses colour groups to make complex layouts easier to inspect.

DYSEKT-SF also supports SFZ authoring:

1. Open **ZONES**.
2. Use **+ ZONE** to add a user sample; create an empty `Custom.sfz` to start a new instrument.
3. Configure the selected zone's `loKey`, `hiKey`, root note, pitch, pan, volume, release, and loop setting.
4. Use **SAVE** to stage changes, or **SAVE AS** to write a new SFZ file.

### SF2 Player

Open an SF2 SoundFont and select from its available presets. The browser displays instrument programs, while the upper displays identify the loaded SoundFont, bank, selected preset, and envelope.

1. Choose **SF2 PLAYER**.
2. Load an SF2 file.
3. Pick a preset in the browser.
4. Play it from MIDI.

## Mixer

Open **MIXER** for a channel-style view of slices, instrument rows, and the master output. Each row provides:

- `GAIN`
- `PAN`
- `FCUT` — filter cutoff
- `PRES` — filter resonance
- `MUTE GRP`
- `CHRO`
- `LEGATO`
- `OUT`

Stereo peak meters are shown at the right. Use the **MASTER** row for overall gain and pan. Drag numeric values vertically to edit, hold **Shift** for finer changes, or double-click a value to enter it directly.

## Global EQ

**GLOBAL EQ** is a five-band equalizer for the complete output, independent of the SFZ Player ADSR controls. Drag a node to adjust its frequency and gain; double-click it to restore its default frequency and 0 dB gain.

The bands cover low shelf, low-mid, mid, high-mid, and high shelf ranges.

## Using DYSEKT-SF in a DAW

1. Add DYSEKT-SF as an instrument plug-in.
2. Route MIDI to the instrument.
3. Select the workspace that matches your source material.

| Goal | Workspace |
| --- | --- |
| Chop or reshape a recording | **Slicer** |
| Play or build a mapped sample instrument | **SFZ Player** |
| Play an existing SoundFont library | **SF2 Player** |

**Tip:** the left panel is your status display, the large right display is the visual editor, and the lower section provides the active workspace's performance or selection tools.

## Build from source

### Requirements

- CMake 3.22 or later
- A compiler with C++20 support
- Git and the repository submodules
- FluidSynth for SF2 live playback (or a local `fluidsynth/` source directory)

### Build

```bash
git clone --recurse-submodules https://github.com/solocky-coder/METRO-UI.git
cd METRO-UI
cmake -S . -B build
cmake --build build --config Release
```

The project uses JUCE and builds the VST3 plug-in plus the standalone application. The optional Ableton Link integration is enabled when its SDK is present in `link/`.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
