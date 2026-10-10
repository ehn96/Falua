# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.22)
project(Falua VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
endif()

# --- JUCE: a local checkout (-DFALUA_JUCE_DIR=/path/to/JUCE) or downloaded once ----------------
set(FALUA_JUCE_DIR "" CACHE PATH "Path to a JUCE checkout (empty: download JUCE 8.0.4)")
if(FALUA_JUCE_DIR)
    add_subdirectory(${FALUA_JUCE_DIR} juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(juce)
endif()

# --- the firmware's sound engines as a C library -----------------------------------------------
set(JIANT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jiant)
add_library(jiant_core STATIC core/jiant_core.c)
target_include_directories(jiant_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/core
    PRIVATE ${JIANT_DIR}/generated ${JIANT_DIR}/firmware/src)
set_target_properties(jiant_core PROPERTIES C_STANDARD 99 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
if(MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The firmware is GNU C: on Windows configure with the clang-cl toolset: cmake -T ClangCL ...")
endif()
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(jiant_core PRIVATE /clang:-ffp-contract=off /clang:-w /clang:-O2)
else()
    target_compile_options(jiant_core PRIVATE -ffp-contract=off -w -O2)
endif()

# --- the plugin ---------------------------------------------------------------------------------
set(FALUA_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND FALUA_FORMATS AU)
endif()

juce_add_plugin(Falua
    COMPANY_NAME "Falua"
    BUNDLE_ID "org.falua.synth"
    PLUGIN_MANUFACTURER_CODE Falu
    PLUGIN_CODE Fal1
    FORMATS ${FALUA_FORMATS}
    PRODUCT_NAME "Falua"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Falua PRIVATE
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp)

target_compile_definitions(Falua PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Falua
    PRIVATE jiant_core juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# --- tests: the core through its C API (no JUCE) -------------------------------------------------
option(FALUA_TESTS "Build the core test" ON)
if(FALUA_TESTS)
    add_executable(core_test tests/core_test.c)
    target_link_libraries(core_test PRIVATE jiant_core)
    if(NOT WIN32)
        target_link_libraries(core_test PRIVATE m)
    endif()
endif()

# --- host test: loads the built VST3 like a DAW (-DFALUA_HOST_TEST=ON) ---------------------------
option(FALUA_HOST_TEST "Build the VST3 host test" OFF)
if(FALUA_HOST_TEST)
    juce_add_console_app(host_test PRODUCT_NAME "host_test")
    target_sources(host_test PRIVATE tests/host_test.cpp)
    target_compile_definitions(host_test PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(host_test PRIVATE juce::juce_audio_processors juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags)
endif()
