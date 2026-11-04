cmake_minimum_required(VERSION 3.22)

project(Stilla VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary (Apple Silicon + Intel) og macOS 10.13+
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# JUCE: bruk lokal kopi hvis JUCE_DIR er satt, ellers lastes den ned automatisk
set(JUCE_DIR "" CACHE PATH "Sti til en lokal JUCE-mappe (valgfritt)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.5
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND PLUGIN_FORMATS AU)
endif()

juce_add_plugin(Stilla
    COMPANY_NAME "Trond Morset"
    BUNDLE_ID "no.trondmorset.stilla"
    PLUGIN_MANUFACTURER_CODE Tmor
    PLUGIN_CODE Stla
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "Stilla"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/stilla-logo-1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/stilla-logo-256.png"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Restoration
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_add_binary_data(StillaAssets SOURCES assets/stilla-logo-256.png)

target_sources(Stilla PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Stilla PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Stilla
    PRIVATE
        StillaAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Valgfrie tester (av som standard): cmake -DSTILLA_BUILD_TESTS=ON
option(STILLA_BUILD_TESTS "Bygg testprogrammer" OFF)
if(STILLA_BUILD_TESTS)
    # DSP-kjernen har ingen JUCE-avhengighet
    add_executable(DspTest Tests/DspTest.cpp)
    target_include_directories(DspTest PRIVATE Source)
    juce_add_console_app(GuiSnapshot PRODUCT_NAME "GuiSnapshot")
    target_sources(GuiSnapshot PRIVATE Tests/GuiSnapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(GuiSnapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Stilla")
    target_link_libraries(GuiSnapshot PRIVATE StillaAssets juce::juce_audio_utils juce::juce_dsp)
endif()

# Vert-test som laster selve .vst3-fila (brukes lokalt og i CI)
if(STILLA_BUILD_TESTS)
    juce_add_console_app(HostTest PRODUCT_NAME "HostTest")
    target_sources(HostTest PRIVATE Tests/HostTest.cpp)
    target_compile_definitions(HostTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(HostTest PRIVATE juce::juce_audio_utils)
endif()
