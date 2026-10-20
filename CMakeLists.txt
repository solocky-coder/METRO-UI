cmake_minimum_required(VERSION 3.22)
project(Avix VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/JUCE JUCE)

# --- AOO / SonoBus network audio ---
# Network audio is a AVIX standalone feature. The plugin target must remain
# free of AOO/SonoBus transport and network-audio sources.
option(AVIX_ENABLE_AOO "Enable AOO/SonoBus network audio in AVIX standalone" ON)
if (AVIX_ENABLE_AOO)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MetroAoo.cmake)
else()
    set(AVIX_HAS_AOO FALSE)
endif()

# --- chowdsp_utils (SVF EQ filters — used by VoicePool per-slice EQ) ---
include(FetchContent)
FetchContent_Declare(
    chowdsp_utils
    GIT_REPOSITORY https://github.com/Chowdhury-DSP/chowdsp_utils
    GIT_TAG        v2.3.0
    GIT_SHALLOW    TRUE
)
set(CHOWDSP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(chowdsp_utils)

# --- tracktion_engine REMOVED ---
# tracktion_engine does not compile against JUCE 8.0.12
# (tracktion_PluginWindowState.cpp references juce::Displays::Display::userBounds
# which was removed in JUCE 7+). SequencerEngine.cpp now uses AVIX's own
# tick scheduler exclusively. MidiClip.cpp keeps the te::MidiList* hook as a
# forward-declared pointer but all midiList calls are no-ops until tracktion
# is re-enabled against a compatible JUCE version.

# --- Ableton Link (tempo sync across apps on LAN/WiFi) ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/link/include/ableton/Link.hpp")
    set(AVIX_HAS_LINK TRUE)
    message(STATUS "Ableton Link SDK found — Link sync enabled")
else()
    set(AVIX_HAS_LINK FALSE)
    message(STATUS "Ableton Link SDK NOT found — Link sync disabled.")
    message(STATUS "  To enable: git submodule add https://github.com/Ableton/link.git link")
endif()

# --- sfizz (SFZ/SF2 engine — SoundFontLoader slice-render + SfzPlayer live SFZ playback) ---
if(TRUE)
    set(SFIZZ_CLIENTS    OFF CACHE BOOL "" FORCE)
    set(SFIZZ_SHARED     OFF CACHE BOOL "" FORCE)
    set(SFIZZ_JACK       OFF CACHE BOOL "" FORCE)
    set(SFIZZ_LV2        OFF CACHE BOOL "" FORCE)
    set(SFIZZ_VST        OFF CACHE BOOL "" FORCE)
    set(SFIZZ_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(SFIZZ_TESTS      OFF CACHE BOOL "" FORCE)
    set(SFIZZ_RENDER     OFF CACHE BOOL "" FORCE)
    add_subdirectory(sfizz EXCLUDE_FROM_ALL)
    set(AVIX_HAS_SFIZZ TRUE)
    message(STATUS "sfizz found — SF2/SFZ slice-render support enabled")
else()
    set(AVIX_HAS_SFIZZ FALSE)
endif()

# --- FluidSynth ---
find_package(FluidSynth CONFIG QUIET)
if(FluidSynth_FOUND)
    set(AVIX_HAS_FLUIDSYNTH TRUE)
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fluidsynth/CMakeLists.txt")
        set(FLUIDSYNTH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FLUIDSYNTH_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(enable-dbus OFF CACHE BOOL "" FORCE)
        set(enable-jack OFF CACHE BOOL "" FORCE)
        set(enable-pulseaudio OFF CACHE BOOL "" FORCE)
        set(enable-alsa OFF CACHE BOOL "" FORCE)
        set(enable-coreaudio OFF CACHE BOOL "" FORCE)
        set(enable-portaudio OFF CACHE BOOL "" FORCE)
        set(enable-openmp OFF CACHE BOOL "" FORCE)
        add_subdirectory(fluidsynth EXCLUDE_FROM_ALL)
        set(AVIX_HAS_FLUIDSYNTH TRUE)
    else()
        set(AVIX_HAS_FLUIDSYNTH FALSE)
    endif()
endif()

option(AVIX_SF2_EXPERIMENTAL_MULTI_GROUP
       "EXPERIMENTAL: FluidSynth audio-groups=16 multi-channel render test"
       OFF)

if(AVIX_HAS_FLUIDSYNTH)
    add_executable(audio_groups_repro diagnostics/audio_groups_repro.c)
    target_link_libraries(audio_groups_repro PRIVATE FluidSynth::libfluidsynth)
endif()

if(AVIX_HAS_SFIZZ)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz.h")
        set(SFIZZ_C_API_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz/sfizz.h")
        set(SFIZZ_C_API_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz")
    else()
        set(SFIZZ_C_API_INCLUDE "")
    endif()
endif()

juce_add_plugin(Avix
    COMPANY_NAME "DunSoft"
    PLUGIN_MANUFACTURER_CODE Dnst
    # PLUGIN_CODE is an internal 4-char ID, never shown to users. It is
    # deliberately unchanged from the pre-rebrand build: changing it changes
    # the plugin's identity for DAWs, so existing sessions would no longer
    # find it. Change it (any 4 chars, at least one uppercase) only if you
    # want a clean break from the old identity.
    PLUGIN_CODE Dysk
    FORMATS VST3
    PRODUCT_NAME "AVIX"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

if(MSVC)
    target_compile_options(Avix PRIVATE /utf-8)
endif()

juce_add_binary_data(AvixFonts SOURCES
    docs/AVIX_User_Manual.pdf
    src/ui/fonts/BarlowCondensed-Regular.ttf
    src/ui/fonts/BarlowCondensed-SemiBold.ttf
    src/ui/fonts/JetBrainsMono-Regular.ttf
    src/ui/fonts/JetBrainsMono-Bold.ttf
    src/ui/assets/Empty.wav
    src/ui/assets/icons/icon_mute.svg
    src/ui/assets/icons/icon_solo.svg
    src/ui/assets/icons/icon_lock.svg
    src/ui/assets/icons/icon_power.svg
    src/ui/assets/icons/icon_pan.svg
    src/ui/assets/icons/icon_volume.svg
    src/ui/assets/buttons/button_idle.svg
    src/ui/assets/buttons/button_hover.svg
    src/ui/assets/buttons/button_active.svg
    src/ui/assets/knobs/knob_face.svg
    src/ui/assets/knobs/knob_pointer.svg
    src/ui/assets/knobs/knob_sprite_64x64_64frames.svg
    src/ui/assets/knobs/knob_sprite_64x64_64frames.png
    src/ui/assets/logos/AVIX_logo.png
    src/ui/assets/logos/AVIX_icon.png
)

target_sources(Avix PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/audio/SampleData.cpp
    src/audio/SliceManager.cpp
    src/audio/VoicePool.cpp
    src/audio/GrainEngine.cpp
    src/audio/LazyChopEngine.cpp
    src/audio/SoundFontLoader.cpp
    src/audio/SfzPlayer.cpp
    src/params/ParamLayout.cpp
    src/ui/AvixLookAndFeel.cpp
    src/ui/IconManager.cpp
    src/metro/MetroTheme.cpp
    src/metro/MetroLookAndFeel.cpp
    src/metro/XivaTheme.cpp
    src/metro/XivaLookAndFeel.cpp
    src/ui/HeaderBar.cpp
    src/ui/SliceControlBar.cpp
    src/ui/WaveformView.cpp
    src/ui/WaveformCache.cpp
    src/ui/SliceLane.cpp
    src/ui/ActionPanel.cpp
    src/ui/LogoBar.cpp
    src/ui/KeysPanel.cpp
    src/ui/FileBrowserPanel.cpp
    src/ui/SfzFileBrowser.cpp
    src/ui/SliceLcdDisplay.cpp
    src/ui/TrimDialog.cpp
    src/ui/MidiLearnDialog.cpp
    src/ui/MixerPanel.cpp
    src/ui/ShortcutsPanel.cpp
    src/ui/SliceWaveformLcd.cpp
    src/ui/MultisamplerWaveformLcd.cpp
    src/ui/Sf2LcdDisplay.cpp
    src/ui/Sf2WaveformLcd.cpp
    src/ui/DualLcdControlFrame.cpp
    src/ui/Sf2InstrumentWorkspace.cpp
    src/ui/SfzPlayerDropdownPanel.cpp
    src/ui/Sf2ProgramGrid.cpp
    src/ui/SfzModulePanel.cpp
    src/ui/WaveformOverview.cpp
    src/ui/ArchiveIntegration.cpp
    src/ui/GlobalEqPanel.cpp
    src/ui/multisampler/MultisamplerEditor.cpp
    src/ui/multisampler/DrumPadView.cpp
    src/ui/multisampler/MultisamplerZoneLcd.cpp
    src/ui/multisampler/AddZoneTrimOverlay.cpp
    src/ui/multisampler/ZoneMapView.cpp
    src/audio/multisampler/MultisamplerInstrument.cpp
    src/audio/multisampler/InstrumentSerializer.cpp
    src/audio/multisampler/SfzImporter.cpp
    src/audio/multisampler/SfzExporter.cpp
    src/sequencer/SequencerEngine.cpp
    src/sequencer/MidiClip.cpp
)

target_include_directories(Avix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<$<BOOL:${AVIX_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/include>
    $<$<BOOL:${AVIX_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/modules/asio-standalone/asio/include>
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear
    $<$<BOOL:${AVIX_HAS_SFIZZ}>:${SFIZZ_C_API_INCLUDE}>
)

target_compile_definitions(Avix PUBLIC
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    AVIX_HAS_AOO=0
    AVIX_HAS_LINK=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_WEB_BROWSER=0
    AVIX_HAS_SFIZZ=$<BOOL:${AVIX_HAS_SFIZZ}>
    AVIX_HAS_FLUIDSYNTH=$<BOOL:${AVIX_HAS_FLUIDSYNTH}>
    AVIX_STANDALONE=0
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Windows>>:LINK_PLATFORM_WINDOWS=1>
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Darwin>>:LINK_PLATFORM_MACOSX=1>
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Linux>>:LINK_PLATFORM_LINUX=1>
)

if (AVIX_SF2_EXPERIMENTAL_MULTI_GROUP)
    target_compile_definitions(Avix PUBLIC AVIX_SF2_EXPERIMENTAL_MULTI_GROUP=1)
endif()

target_link_libraries(Avix
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        AvixFonts
        $<$<BOOL:${AVIX_HAS_SFIZZ}>:sfizz::sfizz>
        $<$<BOOL:${AVIX_HAS_FLUIDSYNTH}>:FluidSynth::libfluidsynth>
        $<$<PLATFORM_ID:Linux>:asound>
        chowdsp::chowdsp_filters
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

juce_add_gui_app(AvixStandalone
    PRODUCT_NAME "AVIX"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/AVIX.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/AVIX.png"
)

if(MSVC)
    target_compile_options(AvixStandalone PRIVATE /utf-8)
    # AvixStandalone itself no longer needs to run elevated: Apple USB
    # Share's driver/PnP work now lives entirely in the separate
    # iPhoneUsbShare.exe helper (see AppleUsbShareLauncher and
    # docs/APPLE_USB_SHARE_STANDALONE_MIGRATION.md), which requests
    # elevation itself via ShellExecuteExW("runas"). Requiring the whole
    # DAW-hosted standalone/plugin to run as Administrator just for one
    # feature is what this migration removes.
endif()

target_sources(AvixStandalone PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/audio/SampleData.cpp
    src/audio/SliceManager.cpp
    src/audio/VoicePool.cpp
    src/audio/GrainEngine.cpp
    src/audio/LazyChopEngine.cpp
    src/audio/SoundFontLoader.cpp
    src/audio/SfzPlayer.cpp
    src/params/ParamLayout.cpp
    src/ui/AvixLookAndFeel.cpp
    src/ui/IconManager.cpp
    src/metro/MetroTheme.cpp
    src/metro/MetroLookAndFeel.cpp
    src/metro/XivaTheme.cpp
    src/metro/XivaLookAndFeel.cpp
    src/ui/HeaderBar.cpp
    src/ui/SliceControlBar.cpp
    src/ui/WaveformView.cpp
    src/ui/WaveformCache.cpp
    src/ui/SliceLane.cpp
    src/ui/ActionPanel.cpp
    src/ui/LogoBar.cpp
    src/ui/KeysPanel.cpp
    src/ui/FileBrowserPanel.cpp
    src/ui/SfzFileBrowser.cpp
    src/ui/SliceLcdDisplay.cpp
    src/ui/TrimDialog.cpp
    src/ui/MidiLearnDialog.cpp
    src/ui/MixerPanel.cpp
    src/ui/ShortcutsPanel.cpp
    src/ui/SliceWaveformLcd.cpp
    src/ui/MultisamplerWaveformLcd.cpp
    src/ui/Sf2LcdDisplay.cpp
    src/ui/Sf2WaveformLcd.cpp
    src/ui/DualLcdControlFrame.cpp
    src/ui/Sf2InstrumentWorkspace.cpp
    src/ui/SfzPlayerDropdownPanel.cpp
    src/ui/Sf2ProgramGrid.cpp
    src/ui/SfzModulePanel.cpp
    src/ui/WaveformOverview.cpp
    src/ui/ArchiveIntegration.cpp
    src/ui/GlobalEqPanel.cpp
    src/ui/multisampler/MultisamplerEditor.cpp
    src/ui/multisampler/DrumPadView.cpp
    src/ui/multisampler/MultisamplerZoneLcd.cpp
    src/ui/multisampler/AddZoneTrimOverlay.cpp
    src/ui/multisampler/ZoneMapView.cpp
    src/audio/multisampler/MultisamplerInstrument.cpp
    src/audio/multisampler/InstrumentSerializer.cpp
    src/audio/multisampler/SfzImporter.cpp
    src/audio/multisampler/SfzExporter.cpp
    src/sequencer/SequencerEngine.cpp
    src/sequencer/MidiClip.cpp
    src/ui/FloatingTransportBar.cpp
    src/standalone/Main.cpp
    src/standalone/MainWindow.cpp
    src/standalone/network/DeviceNetworkTransport.cpp
    src/standalone/network/AppleUsbNetworkTransport.cpp
    src/standalone/network/AppleUsbShareLauncher.cpp
    src/network/RtpMidiSession.cpp
    $<$<BOOL:${AVIX_HAS_AOO}>:src/network/MetroNetworkAudio.cpp>
)

target_include_directories(AvixStandalone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<$<BOOL:${AVIX_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/include>
    $<$<BOOL:${AVIX_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/modules/asio-standalone/asio/include>
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear
    $<$<BOOL:${AVIX_HAS_SFIZZ}>:${SFIZZ_C_API_INCLUDE}>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common>
)

target_compile_definitions(AvixStandalone PUBLIC
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    AVIX_HAS_AOO=$<BOOL:${AVIX_HAS_AOO}>
    AVIX_HAS_LINK=$<BOOL:${AVIX_HAS_LINK}>
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_WEB_BROWSER=0
    AVIX_HAS_SFIZZ=$<BOOL:${AVIX_HAS_SFIZZ}>
    AVIX_HAS_FLUIDSYNTH=$<BOOL:${AVIX_HAS_FLUIDSYNTH}>
    AVIX_STANDALONE=1
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Windows>>:LINK_PLATFORM_WINDOWS=1>
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Darwin>>:LINK_PLATFORM_MACOSX=1>
    $<$<AND:$<BOOL:${AVIX_HAS_LINK}>,$<PLATFORM_ID:Linux>>:LINK_PLATFORM_LINUX=1>
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
    $<$<PLATFORM_ID:Windows>:WINVER=0x0A00>
)

if (AVIX_SF2_EXPERIMENTAL_MULTI_GROUP)
    target_compile_definitions(AvixStandalone PUBLIC AVIX_SF2_EXPERIMENTAL_MULTI_GROUP=1)
endif()

target_link_libraries(AvixStandalone
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_dsp
        AvixFonts
        $<$<BOOL:${AVIX_HAS_AOO}>:MetroAoo>
        $<$<BOOL:${AVIX_HAS_SFIZZ}>:sfizz::sfizz>
        $<$<BOOL:${AVIX_HAS_FLUIDSYNTH}>:FluidSynth::libfluidsynth>
        $<$<PLATFORM_ID:Linux>:asound>
        chowdsp::chowdsp_filters
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(WIN32)
    # Apple USB Share's elevated helper is a self-contained single-file
    # publish (~150+ MB) — far too large to compile into a BinaryData
    # C-array (and, separately, too large for a plain git push; see
    # AppleUsbShareLauncher.h). It ships as a loose file next to
    # AvixStandalone.exe instead: AppleUsbShareLauncher locates it via
    # juce::File::currentApplicationFile, which only resolves correctly
    # for the Standalone target, not a VST3 hosted inside a DAW — hence
    # this whole block being WIN32-and-AvixStandalone-only.
    set(AVIX_APPLE_USB_HELPER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/standalone/network/AppleUsbShareDriver/iPhoneUsbShare.exe")
    if(EXISTS "${AVIX_APPLE_USB_HELPER_SRC}")
        add_custom_command(TARGET AvixStandalone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:AvixStandalone>/AppleUsbShareDriver"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AVIX_APPLE_USB_HELPER_SRC}"
                "$<TARGET_FILE_DIR:AvixStandalone>/AppleUsbShareDriver/iPhoneUsbShare.exe"
            COMMENT "Copying iPhoneUsbShare.exe next to AvixStandalone"
        )
    else()
        message(WARNING
            "Apple USB Share helper not found at ${AVIX_APPLE_USB_HELPER_SRC} — "
            "publish iPhoneUsbShare (dotnet publish -c Release -r win-x64 "
            "--self-contained true -p:PublishSingleFile=true) and place the "
            "resulting exe there. AppleUsbShareLauncher::start() will fail at "
            "runtime until this exists; the rest of the build is unaffected.")
    endif()
endif()
