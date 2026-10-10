// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PluginProcessor.h"

// v0.1: a header with the preset list, and every parameter as a slider (JUCE's generic editor).
class FaluaEditor : public juce::AudioProcessorEditor
{
public:
    explicit FaluaEditor (FaluaProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    FaluaProcessor& proc;
    juce::Label title, engineLabel, warning;
    juce::ComboBox presets;
    juce::GenericAudioProcessorEditor generic;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FaluaEditor)
};
