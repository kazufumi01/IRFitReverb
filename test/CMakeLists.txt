cmake_minimum_required(VERSION 3.22)
project(IRFitTest VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT PLUG)
  set(PLUG "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

if(NOT JUCE_PATH)
  if(EXISTS "$ENV{HOME}/Documents/jpkplugins/JUCE")
    set(JUCE_PATH "$ENV{HOME}/Documents/jpkplugins/JUCE")
  elseif(EXISTS "/home/claude/JUCE")
    set(JUCE_PATH "/home/claude/JUCE")
  endif()
endif()

if(JUCE_PATH)
  add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE)
else()
  include(FetchContent)
  FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

juce_add_console_app(IRFitTest PRODUCT_NAME "IRFitTest")
target_sources(IRFitTest PRIVATE
  irfit_test.cpp
  ${PLUG}/Source/IRFitProcessor.cpp
  ${PLUG}/Source/IRFitEditor.cpp
  ${PLUG}/Source/IRFitConvert.cpp
  ${PLUG}/Source/PluginProcessor.cpp
  ${PLUG}/Source/PluginEditor.cpp)
target_include_directories(IRFitTest PRIVATE ${PLUG}/Source)
target_compile_definitions(IRFitTest PRIVATE
  IRFIT_PRODUCT=1
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JucePlugin_Name="IR Fit Reverb"
  JucePlugin_VersionString="1.0.0"
  JUCE_STANDALONE_APPLICATION=1
  JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(IRFitTest PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
