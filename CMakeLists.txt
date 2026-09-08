cmake_minimum_required(VERSION 3.22)
project(Dysekt VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/JUCE JUCE)

# --- AOO / SonoBus network audio ---
option(DYSEKT_ENABLE_AOO "Enable AOO/SonoBus network audio" ON)
if (DYSEKT_ENABLE_AOO)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MetroAoo.cmake)
else()
    set(DYSEKT_HAS_AOO FALSE)
endif()

include(FetchContent)
FetchContent_Declare(
    chowdsp_utils
    GIT_REPOSITORY https://github.com/Chowdhury-DSP/chowdsp_utils
    GIT_TAG        v2.3.0
    GIT_SHALLOW    TRUE
)
set(CHOWDSP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(chowdsp_utils)

# --- Ableton Link ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/link/include/ableton/Link.hpp")
    set(DYSEKT_HAS_LINK TRUE)
    message(STATUS "Ableton Link SDK found — Link sync enabled")
else()
    set(DYSEKT_HAS_LINK FALSE)
    message(STATUS "Ableton Link SDK NOT found — Link sync disabled.")
endif()

# --- sfizz ---
set(SFIZZ_CLIENTS OFF CACHE BOOL "" FORCE)
set(SFIZZ_SHARED OFF CACHE BOOL "" FORCE)
set(SFIZZ_JACK OFF CACHE BOOL "" FORCE)
set(SFIZZ_LV2 OFF CACHE BOOL "" FORCE)
set(SFIZZ_VST OFF CACHE BOOL "" FORCE)
set(SFIZZ_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SFIZZ_TESTS OFF CACHE BOOL "" FORCE)
set(SFIZZ_RENDER OFF CACHE BOOL "" FORCE)
add_subdirectory(sfizz EXCLUDE_FROM_ALL)
set(DYSEKT_HAS_SFIZZ TRUE)

# --- FluidSynth ---
find_package(FluidSynth CONFIG QUIET)
if(FluidSynth_FOUND)
    set(DYSEKT_HAS_FLUIDSYNTH TRUE)
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
        set(DYSEKT_HAS_FLUIDSYNTH TRUE)
    else()
        set(DYSEKT_HAS_FLUIDSYNTH FALSE)
    endif()
endif()

if(DYSEKT_HAS_FLUIDSYNTH)
    add_executable(audio_groups_repro diagnostics/audio_groups_repro.c)
    target_link_libraries(audio_groups_repro PRIVATE FluidSynth::libfluidsynth)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz.h")
    set(SFIZZ_C_API_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz/sfizz.h")
    set(SFIZZ_C_API_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/sfizz/src/sfizz")
else()
    set(SFIZZ_C_API_INCLUDE "")
endif()

juce_add_plugin(Dysekt
    COMPANY_NAME "DunSoft"
    PLUGIN_MANUFACTURER_CODE Dnst
    PLUGIN_CODE Dysk
    FORMATS VST3
    PRODUCT_NAME "DYSEKT-SF"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

if(MSVC)
    target_compile_options(Dysekt PRIVATE /source-charset:utf-8)
endif()

juce_add_binary_data(DysektFonts SOURCES
    docs/DYSEKT_User_Manual.pdf
    src/ui/fonts/BarlowCondensed-Regular.ttf
    src/ui/fonts/BarlowCondensed-SemiBold.ttf
    src/ui/fonts/JetBrainsMono-Regular.ttf
    src/ui/fonts/JetBrainsMono-Bold.ttf
    src/ui/assets/dysekt_logo.svg
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
    src/ui/assets/logos/DYSEKT-SF_logo_cr8.png
    src/ui/assets/logos/DYSEKT-SF_logo_dark.png
    src/ui/assets/logos/DYSEKT-SF_logo_dysekt.png
    src/ui/assets/logos/DYSEKT-SF_logo_ghost.png
    src/ui/assets/logos/DYSEKT-SF_logo_hack.png
    src/ui/assets/logos/DYSEKT-SF_logo_lazy.png
    src/ui/assets/logos/DYSEKT-SF_logo_midnight.png
    src/ui/assets/logos/DYSEKT-SF_logo_opendaw.png
    src/ui/assets/logos/DYSEKT-SF_logo_pigments.png
    src/ui/assets/logos/DYSEKT-SF_logo_shell.png
    src/ui/assets/logos/DYSEKT-SF_logo_snow.png
)

set(DYSEKT_COMMON_SOURCES
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
    src/ui/DysektLookAndFeel.cpp
    src/ui/IconManager.cpp
    src/metro/MetroTheme.cpp
    src/metro/MetroLookAndFeel.cpp
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
    src/ui/PadGridView.cpp
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
    src/ui/multisampler/MultisamplerZoneLcd.cpp
    src/ui/multisampler/AddZoneTrimOverlay.cpp
    src/ui/multisampler/ZoneMapView.cpp
    src/audio/multisampler/MultisamplerInstrument.cpp
    src/audio/multisampler/InstrumentSerializer.cpp
    src/audio/multisampler/SfzImporter.cpp
    src/audio/multisampler/SfzExporter.cpp
    src/sequencer/SequencerEngine.cpp
    src/sequencer/SequencerEngineNetworkAudio.cpp
    src/sequencer/MidiClip.cpp
    $<$<BOOL:${DYSEKT_HAS_AOO}>:src/network/MetroNetworkAudio.cpp>
)

target_sources(Dysekt PRIVATE ${DYSEKT_COMMON_SOURCES})
target_include_directories(Dysekt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<$<BOOL:${DYSEKT_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/include>
    $<$<BOOL:${DYSEKT_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/modules/asio-standalone/asio/include>
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear
    $<$<BOOL:${DYSEKT_HAS_SFIZZ}>:${SFIZZ_C_API_INCLUDE}>
)

target_compile_definitions(Dysekt PUBLIC
    JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1
    DYSEKT_HAS_AOO=$<BOOL:${DYSEKT_HAS_AOO}>
    DYSEKT_HAS_LINK=0
    JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_WEB_BROWSER=0
    DYSEKT_HAS_SFIZZ=$<BOOL:${DYSEKT_HAS_SFIZZ}>
    DYSEKT_HAS_FLUIDSYNTH=$<BOOL:${DYSEKT_HAS_FLUIDSYNTH}>
    DYSEKT_STANDALONE=0
)

target_link_libraries(Dysekt PRIVATE juce::juce_audio_utils juce::juce_dsp DysektFonts
    $<$<BOOL:${DYSEKT_HAS_AOO}>:MetroAoo>
    $<$<BOOL:${DYSEKT_HAS_SFIZZ}>:sfizz::sfizz>
    $<$<BOOL:${DYSEKT_HAS_FLUIDSYNTH}>:FluidSynth::libfluidsynth>
    $<$<PLATFORM_ID:Linux>:asound> chowdsp::chowdsp_filters
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

juce_add_gui_app(DysektStandalone PRODUCT_NAME "DYSEKT-SF"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/DYSEKT-SF.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/DYSEKT-SF.png")
if(MSVC)
    target_compile_options(DysektStandalone PRIVATE /source-charset:utf-8)
endif()

target_sources(DysektStandalone PRIVATE
    ${DYSEKT_COMMON_SOURCES}
    src/ui/FloatingTransportBar.cpp
    src/standalone/Main.cpp
    src/standalone/MainWindow.cpp
)

target_include_directories(DysektStandalone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<$<BOOL:${DYSEKT_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/include>
    $<$<BOOL:${DYSEKT_HAS_LINK}>:${CMAKE_CURRENT_SOURCE_DIR}/link/modules/asio-standalone/asio/include>
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear
    $<$<BOOL:${DYSEKT_HAS_SFIZZ}>:${SFIZZ_C_API_INCLUDE}>
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common>
)

target_compile_definitions(DysektStandalone PUBLIC
    JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1
    DYSEKT_HAS_AOO=$<BOOL:${DYSEKT_HAS_AOO}>
    DYSEKT_HAS_LINK=$<BOOL:${DYSEKT_HAS_LINK}>
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_WEB_BROWSER=0
    DYSEKT_HAS_SFIZZ=$<BOOL:${DYSEKT_HAS_SFIZZ}>
    DYSEKT_HAS_FLUIDSYNTH=$<BOOL:${DYSEKT_HAS_FLUIDSYNTH}>
    DYSEKT_STANDALONE=1
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
)

target_link_libraries(DysektStandalone PRIVATE juce::juce_audio_utils juce::juce_dsp DysektFonts
    $<$<BOOL:${DYSEKT_HAS_AOO}>:MetroAoo>
    $<$<BOOL:${DYSEKT_HAS_SFIZZ}>:sfizz::sfizz>
    $<$<BOOL:${DYSEKT_HAS_FLUIDSYNTH}>:FluidSynth::libfluidsynth>
    $<$<PLATFORM_ID:Linux>:asound> chowdsp::chowdsp_filters
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
