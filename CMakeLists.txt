cmake_minimum_required(VERSION 3.22)

project(Somnia VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# -----------------------------------------------------------------------------
# Apple linker (ld-prime) workaround
#
# The macOS 27 (Tahoe) linker refuses to LTO-link JUCE's static *_SharedCode
# archive unless the target triple is passed explicitly on the link line.
# -----------------------------------------------------------------------------
if (APPLE)
    if (CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 SOMNIA_OSX_ARCH)
    else()
        set(SOMNIA_OSX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    add_link_options("-target" "${SOMNIA_OSX_ARCH}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# -----------------------------------------------------------------------------
# JUCE (fetched locally into the build tree, nothing installed system-wide)
# -----------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE)

# -----------------------------------------------------------------------------
# Plugin target
# -----------------------------------------------------------------------------
juce_add_plugin(Somnia
    COMPANY_NAME              "Starshinov"
    PLUGIN_MANUFACTURER_CODE  Strv
    PLUGIN_CODE               Smna
    FORMATS                   VST3 AU Standalone
    PRODUCT_NAME              "Somnia"
    BUNDLE_ID                 "com.starshinov.somnia"
    DESCRIPTION               "Avant-garde tape loop recorder / disintegration machine"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE
    PLUGIN_IS_RTAS            FALSE
)

target_sources(Somnia PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DebugLogger.cpp
    Source/ui/SomniaLookAndFeel.cpp
    Source/ui/TapeDisplay.cpp
)

target_include_directories(Somnia PRIVATE Source)

target_compile_definitions(Somnia PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(Somnia
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# -----------------------------------------------------------------------------
# Offline test helpers
#
#   somnia_add_dsp_test        - header-only DSP test (fast, no plugin sources)
#   somnia_add_processors_test - test that needs juce_audio_processors (APVTS)
#   somnia_add_plugin_test     - runs the real processor + editor
# -----------------------------------------------------------------------------
function(somnia_add_dsp_test name source)
    juce_add_console_app(${name} PRODUCT_NAME ${name})
    target_sources(${name} PRIVATE ${source})
    target_link_libraries(${name}
        PRIVATE
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endfunction()

function(somnia_add_processors_test name source)
    juce_add_console_app(${name} PRODUCT_NAME ${name})
    target_sources(${name} PRIVATE ${source})
    target_link_libraries(${name}
        PRIVATE
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endfunction()

function(somnia_add_plugin_test name source)
    juce_add_console_app(${name} PRODUCT_NAME ${name})
    target_sources(${name} PRIVATE
        ${source}
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/DebugLogger.cpp
        Source/ui/SomniaLookAndFeel.cpp
        Source/ui/TapeDisplay.cpp
    )
    target_include_directories(${name} PRIVATE Source)
    target_compile_definitions(${name} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JucePlugin_Name="Somnia"
        # Keeps the black-box log out of the real per-user file.
        SOMNIA_TEST_BUILD=1
    )
    target_link_libraries(${name}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endfunction()

# --- DSP / engine (header-only, fast to build) --------------------------------
somnia_add_dsp_test(SomniaTapeTest     Tests/TapeLoopTest.cpp)
somnia_add_dsp_test(SomniaDegraderTest Tests/DegraderTest.cpp)
somnia_add_dsp_test(SomniaDspTest      Tests/DspStressTest.cpp)

# --- presets ------------------------------------------------------------------
somnia_add_processors_test(SomniaPresetTest Tests/PresetTest.cpp)

# --- full plugin (real processor + editor) ------------------------------------
somnia_add_plugin_test(SomniaPluginTest Tests/PluginTest.cpp)
