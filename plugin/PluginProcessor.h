// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <climits>
#include <vector>

// One parameter of the plugin and where it goes in the firmware.
struct FaluaParam
{
    enum Kind { Edit, Track, Global };
    Kind kind = Edit;
    int  index = 0;                 // Edit: knob 0..7; Track: jc track param id; Global: jc global id
    juce::String id, name;
    int  min = 0, max = 127, def = 0;
    juce::RangedAudioParameter* param = nullptr;
    int  last = INT_MIN;        // the value last sent to the core (audio thread only)
};

class FaluaProcessor : public juce::AudioProcessor
{
public:
    FaluaProcessor();
    ~FaluaProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    // factory presets of the engine, as host programs
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState& getState() { return state; }
    bool ownsCore() const { return ownsTheCore; }
    int engineId() const { return engine; }

    static constexpr int part = 0;     // v0.1: one part, every MIDI channel plays it

private:
    juce::AudioProcessorValueTreeState::ParameterLayout buildLayout();
    void pushParameters();
    void renderCore (float* l, float* r, int n);

    // (declared before state: buildLayout() reads them while state is constructed)
    int engine = 0;                    // the firmware's engine number (0 = ANALOG)
    std::vector<FaluaParam> params;
    juce::AudioProcessorValueTreeState state;
    int currentProgram = 0;
    bool ownsTheCore = false;

    // resampling from the core's fixed 44.1 kHz to the host rate
    double hostRate = 44100.0, ratio = 1.0;
    juce::LagrangeInterpolator interpL, interpR;
    std::vector<float> fifoL, fifoR;
    int fifoCount = 0;
    std::vector<float> scratchR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FaluaProcessor)
};
