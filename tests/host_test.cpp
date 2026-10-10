// SPDX-License-Identifier: GPL-3.0-only
// Loads the built Falua.vst3 the way a DAW does, plays notes at several sample rates and block sizes,
// switches presets, saves and restores its state, and writes what it hears to WAV files.
//   host_test path/to/Falua.vst3 outdir
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr,
                                                                     (unsigned) b.getNumChannels(), 16, {}, 0));
    if (w) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

struct Result { float peak = 0; bool finite = true; };

static Result play (juce::AudioPluginInstance& p, double sr, int block, int program, juce::AudioBuffer<float>& out)
{
    p.setCurrentProgram (program);
    p.prepareToPlay (sr, block);
    const int total = (int) (sr * 2.5);
    out.setSize (2, total);
    out.clear();
    const int notes[] = { 48, 55, 60, 63, 67 };
    Result r;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        juce::MidiBuffer midi;
        for (int i = 0; i < 5; ++i)
        {
            const int on = (int) (sr * (0.1 + 0.3 * i)), off = (int) (sr * (0.35 + 0.3 * i));
            if (on >= pos && on < pos + n)  midi.addEvent (juce::MidiMessage::noteOn (1, notes[i], (juce::uint8) 100), on - pos);
            if (off >= pos && off < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, notes[i]), off - pos);
        }
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
        {
            out.copyFrom (c, pos, buf, c, 0, n);
            for (int i = 0; i < n; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) r.finite = false;
                r.peak = juce::jmax (r.peak, std::abs (v));
            }
        }
    }
    p.releaseResources();
    return r;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3) { std::puts ("host_test Falua.vst3 outdir"); return 2; }
    const juce::File plugin (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    outDir.createDirectory();

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> found;
    fmt.findAllTypesForFile (found, plugin.getFullPathName());
    if (found.isEmpty()) { std::puts ("FAIL: no plugin in that file"); return 1; }
    auto& d = *found[0];
    std::printf ("plugin: %s by %s, instrument=%d, %s\n", d.name.toRawUTF8(), d.manufacturerName.toRawUTF8(),
                 (int) d.isInstrument, d.version.toRawUTF8());

    juce::String err;
    auto inst = fmt.createInstanceFromDescription (d, 48000.0, 512, err);
    if (! inst) { std::printf ("FAIL: %s\n", err.toRawUTF8()); return 1; }
    inst->enableAllBuses();

    int fails = 0;
    std::printf ("%d parameters, %d presets\n", inst->getParameters().size(), inst->getNumPrograms());
    for (auto* prm : inst->getParameters())
        std::printf ("  %-16s = %s\n", prm->getName (32).toRawUTF8(), prm->getCurrentValueAsText().toRawUTF8());

    struct Case { double sr; int block; int program; };
    const Case cases[] = { { 44100, 512, 0 }, { 48000, 480, 4 }, { 96000, 1024, 2 }, { 48000, 37, 8 }, { 88200, 64, 14 } };
    for (auto& c : cases)
    {
        juce::AudioBuffer<float> out;
        const auto r = play (*inst, c.sr, c.block, c.program, out);
        const auto name = inst->getProgramName (c.program);
        const bool ok = r.finite && r.peak > 0.01f && r.peak < 1.5f;
        std::printf ("  %6.0f Hz block %4d preset %-10s peak %.3f %s\n", c.sr, c.block, name.toRawUTF8(), r.peak, ok ? "ok" : "FAIL");
        fails += ! ok;
        writeWav (outDir.getChildFile (juce::String ((int) c.sr) + "_" + name.replace (" ", "_") + ".wav"), out, c.sr);
    }

    // state: change a knob, save, change it again, restore, the knob is back
    auto* cutoff = inst->getParameters()[4];
    cutoff->setValueNotifyingHost (0.25f);
    juce::MemoryBlock saved;
    inst->getStateInformation (saved);
    cutoff->setValueNotifyingHost (0.9f);
    inst->setStateInformation (saved.getData(), (int) saved.getSize());
    const bool stateOk = std::abs (cutoff->getValue() - 0.25f) < 0.02f;
    std::printf ("  state save / restore (%s): %s\n", cutoff->getName (32).toRawUTF8(), stateOk ? "ok" : "FAIL");
    fails += ! stateOk;

    // a second instance must stay silent, not fight over the core
    auto second = fmt.createInstanceFromDescription (d, 48000.0, 512, err);
    if (second)
    {
        juce::AudioBuffer<float> out;
        const auto r = play (*second, 48000, 512, 0, out);
        std::printf ("  second instance peak %.3f (silent expected): %s\n", r.peak, r.peak == 0.0f ? "ok" : "FAIL");
        fails += r.peak != 0.0f;
    }
    second.reset();
    inst.reset();
    std::printf (fails ? "%d FAILED\n" : "all host checks passed\n", fails);
    return fails != 0;
}
