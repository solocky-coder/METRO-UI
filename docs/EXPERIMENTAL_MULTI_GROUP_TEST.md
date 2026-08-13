# Experimental SF2 multi-group test

`DYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP` compiles a supervised diagnostic path for
FluidSynth's 16 audio-group rendering. It is **off by default** and must never
be enabled in a release build.

## Windows quick build

Run:

```bat
build-experimental-multigroup.bat
```

The script uses a separate `build-experimental-multigroup` directory, selects
Debug, and configures CMake with:

```text
-DDYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP=ON
```

Normal `build.bat` behavior is unchanged and continues to produce a Release
build with the experiment disabled.

## Manual CMake build

```bash
cmake -S . -B build-experimental-multigroup \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP=ON
cmake --build build-experimental-multigroup --config Debug
```

For multi-configuration generators such as Visual Studio, `--config Debug` is
required even though `CMAKE_BUILD_TYPE` may be ignored.

## Test procedure

1. Start the Debug standalone build under a debugger or sanitizer.
2. Open the SF2 instrument workspace.
3. Confirm that `EXP: MULTI-GROUP` is visible and initially off.
4. Turn it on.
5. Load a fresh `.sf2` file (or reload the current file).
6. Exercise all MIDI channels while watching for crashes, invalid memory access,
   silence, channel leakage, and incorrect channel peak meters.
7. Turn the toggle off and reload the file to compare against the normal path.

The toggle only changes the next SF2 load. It does not reconfigure a synth that
is already loaded.
