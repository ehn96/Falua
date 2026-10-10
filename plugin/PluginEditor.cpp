// SPDX-License-Identifier: GPL-3.0-only
#include "PluginEditor.h"
#include "jiant_core.h"

FaluaEditor::FaluaEditor (FaluaProcessor& p)
    : AudioProcessorEditor (p), proc (p), generic (p)
{
    title.setText ("FALUA", juce::dontSendNotification);
    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    addAndMakeVisible (title);

    engineLabel.setText (juce::String ("engine: ") + jc_engine_name (p.engineId()), juce::dontSendNotification);
    engineLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (engineLabel);

    for (int i = 0; i < p.getNumPrograms(); ++i)
        presets.addItem (juce::String (i + 1) + "  " + jc_preset_name (p.engineId(), i), i + 1);
    presets.setSelectedId (p.getCurrentProgram() + 1, juce::dontSendNotification);
    presets.onChange = [this] { proc.setCurrentProgram (presets.getSelectedId() - 1); };
    addAndMakeVisible (presets);

    if (! p.ownsCore())
    {
        warning.setText ("Only one Falua can play at a time (v0.1). This instance is silent.",
                         juce::dontSendNotification);
        warning.setColour (juce::Label::textColourId, juce::Colours::orange);
        addAndMakeVisible (warning);
    }

    addAndMakeVisible (generic);
    setResizable (true, true);
    setSize (520, 720);
}

void FaluaEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171a));
}

void FaluaEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (32);
    title.setBounds (top.removeFromLeft (120));
    engineLabel.setBounds (top.removeFromRight (160));
    presets.setBounds (top.reduced (6, 2));
    if (! proc.ownsCore())
        warning.setBounds (area.removeFromTop (24));
    area.removeFromTop (6);
    generic.setBounds (area);
}
