// SPDX-License-Identifier: GPL-3.0-only
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "jiant_core.h"

// The firmware keeps its state in globals: one synth per process. The first instance owns it;
// any other instance stays silent and says so in its window.
static std::atomic<bool> coreTaken { false };

namespace
{
    // friendly names for the FM-1's short labels
    juce::String longName (const juce::String& label)
    {
        static const std::pair<const char*, const char*> names[] = {
            { "WAVE", "Wave" }, { "DTN", "Detune" }, { "MIX", "Osc Mix" }, { "NOIS", "Noise" },
            { "CUT", "Cutoff" }, { "RES", "Resonance" }, { "DRV", "Drive" }, { "INT", "Osc 2 Interval" },
        };
        for (auto& n : names)
            if (label == n.first)
                return n.second;
        return label;
    }

    struct TrackSpec { const char* sym; const char* id; const char* name; };
    const TrackSpec trackSpecs[] = {
        { "ATK", "atk", "Attack" },          { "DEC", "dec", "Decay" },
        { "SUS", "sus", "Sustain" },         { "REL", "rel", "Release" },
        { "ENV_FLT", "envflt", "Env > Filter" },
        { "LFO_RATE", "lforate", "LFO Rate" }, { "LFO_WAVE", "lfowave", "LFO Wave" },
        { "LFO_PIT", "lfopit", "LFO > Pitch" }, { "LFO_FLT", "lfoflt", "LFO > Filter" },
        { "LFO_AMP", "lfoamp", "LFO > Amp" },
        { "VOICE", "voice", "Voice Mode" },  { "GLIDE", "glide", "Glide" },
        { "DIST", "dist", "Distortion" },    { "CHOR", "chorus", "Chorus Send" },
        { "DLY", "delay", "Delay Send" },    { "REV", "reverb", "Reverb Send" },
        { "PAN", "pan", "Pan" },             { "LEVEL", "level", "Level" },
    };
    struct GlobalSpec { const char* sym; const char* id; const char* name; };
    const GlobalSpec globalSpecs[] = {
        { "DTIME", "dtime", "Delay Time" },   { "DFDBK", "dfdbk", "Delay Feedback" },
        { "RSIZE", "rsize", "Reverb Size" },  { "RDAMP", "rdamp", "Reverb Damping" },
        { "RTYPE", "rtype", "Reverb Type" },  { "TUNE", "tune", "Master Tune" },
    };
}

FaluaProcessor::FaluaProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "FALUA", buildLayout())
{
    for (auto& p : params)
        p.param = state.getParameter (p.id);

    bool expected = false;
    ownsTheCore = coreTaken.compare_exchange_strong (expected, true);
    if (ownsTheCore)
    {
        jc_init();
        jc_set_engine (part, engine);
    }
    setCurrentProgram (0);
}

FaluaProcessor::~FaluaProcessor()
{
    if (ownsTheCore)
        coreTaken = false;
}

juce::AudioProcessorValueTreeState::ParameterLayout FaluaProcessor::buildLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    jc_param_info info {};

    auto make = [] (FaluaParam::Kind kind, int index, const juce::String& id, const juce::String& name)
    {
        FaluaParam p;
        p.kind = kind; p.index = index; p.id = id; p.name = name;
        return p;
    };
    auto add = [&] (FaluaParam p, const jc_param_info& in, auto valueName)
    {
        p.min = in.min; p.max = in.max; p.def = in.def;
        if (in.is_enum)
        {
            juce::StringArray choices;
            for (int v = in.min; v <= in.max; ++v)
            {
                const char* s = valueName (v);
                choices.add (s ? juce::String (s) : juce::String (v));
            }
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { p.id, 1 }, p.name, choices, in.def - in.min));
        }
        else
        {
            layout.add (std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { p.id, 1 }, p.name, in.min, in.max, in.def));
        }
        params.push_back (p);
    };

    for (int k = 0; k < JC_EDIT_PARAMS; ++k)
        if (jc_edit_param_info (engine, k, &info))
            add (make (FaluaParam::Edit, k, "e" + juce::String (k), longName (info.label)), info,
                 [this, k] (int v) { return jc_edit_value_name (engine, k, v); });

    for (auto& s : trackSpecs)
    {
        const int pid = jc_track_param_id (s.sym);
        if (pid >= 0 && jc_track_param_info (pid, &info))
            add (make (FaluaParam::Track, pid, s.id, s.name), info,
                 [pid] (int v) { return jc_track_value_name (pid, v); });
    }

    for (auto& s : globalSpecs)
    {
        const int g = jc_global_id (s.sym);
        if (g >= 0 && jc_global_info (g, &info))
            add (make (FaluaParam::Global, g, s.id, s.name), info, [] (int) { return (const char*) nullptr; });
    }
    return layout;
}

bool FaluaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void FaluaProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostRate = sampleRate > 0 ? sampleRate : 44100.0;
    ratio = (double) JC_SAMPLE_RATE / hostRate;
    interpL.reset();
    interpR.reset();
    const int cap = (int) std::ceil (samplesPerBlock * ratio) + 64;
    fifoL.assign ((size_t) juce::jmax (cap, 4096), 0.0f);
    fifoR.assign (fifoL.size(), 0.0f);
    fifoCount = 0;
    scratchR.assign ((size_t) juce::jmax (samplesPerBlock, 4096), 0.0f);
    for (auto& p : params)
        p.last = INT_MIN;                   // send everything again
    if (ownsTheCore)
        jc_all_notes_off();
}

// a parameter's current value as the firmware's integer
static int toInt (const FaluaParam& p)
{
    const float v = p.param->convertFrom0to1 (p.param->getValue());
    if (dynamic_cast<juce::AudioParameterChoice*> (p.param) != nullptr)
        return p.min + juce::roundToInt (v);
    return juce::roundToInt (v);
}

void FaluaProcessor::pushParameters()
{
    for (auto& p : params)
    {
        const int v = toInt (p);
        if (v == p.last)
            continue;
        p.last = v;
        switch (p.kind)
        {
            case FaluaParam::Edit:   jc_set_param (part, jc_edit_param_id (p.index), v); break;
            case FaluaParam::Track:  jc_set_param (part, p.index, v); break;
            case FaluaParam::Global: jc_set_global (p.index, v); break;
        }
    }
}

void FaluaProcessor::renderCore (float* l, float* r, int n)
{
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        jc_render (l, r, n);
        return;
    }
    // the core runs at 44.1 kHz: render into the FIFO what this many host frames need, then resample
    const int need = (int) std::ceil (n * ratio) + 4;
    if ((int) fifoL.size() < need + 64)
    {
        fifoL.resize ((size_t) need + 64);    // (only if the host sends a bigger block than announced)
        fifoR.resize ((size_t) need + 64);
    }
    if (fifoCount < need)
    {
        jc_render (fifoL.data() + fifoCount, fifoR.data() + fifoCount, need - fifoCount);
        fifoCount = need;
    }
    const int used = interpL.process (ratio, fifoL.data(), l, n);
    interpR.process (ratio, fifoR.data(), r, n);
    const int left = fifoCount - used;
    std::memmove (fifoL.data(), fifoL.data() + used, sizeof (float) * (size_t) left);
    std::memmove (fifoR.data(), fifoR.data() + used, sizeof (float) * (size_t) left);
    fifoCount = left;
}

void FaluaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (! ownsTheCore)
    {
        buffer.clear();
        return;
    }
    pushParameters();

    float* l = buffer.getWritePointer (0);
    float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    if (r == nullptr)
    {
        if ((int) scratchR.size() < n)
            scratchR.resize ((size_t) n);
        r = scratchR.data();
    }

    // render up to each MIDI event, then hand the event to the firmware
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (at > pos)
        {
            renderCore (l + pos, r + pos, at - pos);
            pos = at;
        }
        const auto m = meta.getMessage();
        const auto* d = m.getRawData();
        if (m.getRawDataSize() >= 1 && d[0] >= 0x80 && d[0] < 0xF0)
        {
            const uint8_t status = (uint8_t) ((d[0] & 0xF0) | part);     // v0.1: every channel plays part 1
            jc_midi (status, m.getRawDataSize() > 1 ? d[1] : 0, m.getRawDataSize() > 2 ? d[2] : 0);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            jc_all_notes_off();
    }
    if (pos < n)
        renderCore (l + pos, r + pos, n - pos);

    if (buffer.getNumChannels() == 1)                 // mono out: the sum
        for (int i = 0; i < n; ++i)
            l[i] = 0.5f * (l[i] + r[i]);
    for (int c = 2; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);
}

// ------------------------------------------------------------------------------- presets ---
int FaluaProcessor::getNumPrograms() { return juce::jmax (1, jc_preset_count (engine)); }

const juce::String FaluaProcessor::getProgramName (int index)
{
    return juce::String (jc_preset_name (engine, index)).toLowerCase().replace ("_", " ");
}

void FaluaProcessor::setCurrentProgram (int index)
{
    int ids[32], values[32];
    const int count = jc_preset_values (engine, index, ids, values, 32);
    currentProgram = juce::jlimit (0, getNumPrograms() - 1, index);
    for (int i = 0; i < count; ++i)
        for (auto& p : params)
        {
            const bool same = (p.kind == FaluaParam::Edit && jc_edit_param_id (p.index) == ids[i])
                           || (p.kind == FaluaParam::Track && p.index == ids[i]);
            if (! same)
                continue;
            const int v = juce::jlimit (p.min, p.max, values[i]);
            const float plain = dynamic_cast<juce::AudioParameterChoice*> (p.param) != nullptr
                                    ? (float) (v - p.min) : (float) v;
            p.param->setValueNotifyingHost (p.param->convertTo0to1 (plain));
        }
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

// --------------------------------------------------------------------------------- state ---
void FaluaProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto tree = state.copyState();
    tree.setProperty ("program", currentProgram, nullptr);
    tree.setProperty ("engine", engine, nullptr);
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, dest);
}

void FaluaProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (state.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = tree.getProperty ("program", 0);
            state.replaceState (tree);
        }
}

juce::AudioProcessorEditor* FaluaProcessor::createEditor() { return new FaluaEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FaluaProcessor(); }
