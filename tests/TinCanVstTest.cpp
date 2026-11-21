// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// The score played by a VST3 instrument (T-086). Part of TinCanAudioTests. Needs a VST3 instrument on
// this computer: TINCAN_TEST_VST3 (path to a .vst3), else Surge XT where its installer puts it;
// skipped (not failed) when there's none.

#include <JuceHeader.h>
#include "../Source/TinCanScorePlayer.h"
#include "../Source/TinCanLiveInstrument.h"

#include <cstdio>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// T-087: each Instrument strip hears all MIDI keyboards or just its own
static void midiRouterTest()
{
    TinCan::MidiRouter router;
    router.prepare(48000.0);
    router.setDevice(1, "kbd-A");
    router.setDevice(2, "kbd-B");
    MidiBuffer out;
    for (int k = 0; k < 4; ++k) router.pull(k, out, 512);   // start each strip's clock
    auto note = MidiMessage::noteOn(1, 60, (uint8) 100);
    note.setTimeStamp(Time::getMillisecondCounterHiRes() * 0.001);
    router.route("kbd-A", note);
    int got[4] = {};
    for (int k = 0; k < 4; ++k) { router.pull(k, out, 512); got[k] = out.getNumEvents(); }
    CHECK(got[0] == 1);   // all keyboards
    CHECK(got[1] == 1);   // keyboard A
    CHECK(got[2] == 0);   // keyboard B
    CHECK(got[3] == 1);   // all keyboards
    CHECK(router.getDevice(1) == "kbd-A");
    std::printf("  midi router: %s\n", (got[0] == 1 && got[1] == 1 && got[2] == 0 && got[3] == 1) ? "ok" : "FAILED");
}

int runVstTests()
{
    const int before = failures;
    midiRouterTest();
#if JUCE_PLUGINHOST_VST3
    File plugin(SystemStats::getEnvironmentVariable("TINCAN_TEST_VST3", "C:\\Program Files\\Common Files\\VST3\\Surge Synth Team\\Surge XT.vst3"));
    if (!plugin.exists()) { std::printf("  vst3 instrument: skipped (no %s)\n", plugin.getFullPathName().toRawUTF8()); return 0; }

    ScopedJuceInitialiser_GUI gui;   // VST3 plugins expect a message thread
    AudioPluginFormatManager formats;
    formats.addFormat(new VST3PluginFormat());
    OwnedArray<PluginDescription> found;
    VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, plugin.getFullPathName());
    CHECK(found.size() > 0);
    if (found.isEmpty()) return failures;
    if (auto dump = SystemStats::getEnvironmentVariable("TINCAN_TEST_VST3_LIST", {}); dump.isNotEmpty()) {
        // a plugins.xml with just this plugin, for trying the app without a full search
        KnownPluginList list;
        for (auto * d : found) list.addType(*d);
        if (auto xml = list.createXml()) xml->writeTo(File(dump));
    }
    const PluginDescription * desc = nullptr;
    for (auto * d : found) if (d->isInstrument) { desc = d; break; }
    CHECK(desc != nullptr);
    if (!desc) return failures;

    const double rate = 48000.0;
    const int block = 512;
    String error;
    auto inst = formats.createPluginInstance(*desc, rate, block, error);
    CHECK(inst != nullptr);
    if (!inst) { std::printf("  vst3: %s\n", error.toRawUTF8()); return failures; }

    TinCan::ScorePlayer player;
    player.prepare(rate, block);
    CHECK(player.setInstrument(std::move(inst)) == nullptr);
    CHECK(player.getInstrument() != nullptr);

    // four quarter notes at 120 bpm, played without the metronome
    std::vector<TinCan::ScoreNote> notes;
    for (int k = 0; k < 4; ++k) { TinCan::ScoreNote n; n.midi = 60 + k * 2; n.beat = k; n.len = 0.9; notes.push_back(n); }
    player.play(notes, 4.0, 0, false);

    AudioBuffer<float> out(2, block);
    double peak = 0.0, sumSq = 0.0;
    long count = 0;
    for (int b = 0; b < (int) (rate * 2.5) / block; ++b) {
        out.clear();
        player.process(out, 2, block, 120.0, false, 0.0, 0.0, 4);
        for (int c = 0; c < 2; ++c) {
            peak = jmax(peak, (double) out.getMagnitude(c, 0, block));
            for (int i = 0; i < block; ++i) sumSq += out.getSample(c, i) * out.getSample(c, i);
            count += block;
        }
    }
    const double rms = std::sqrt(sumSq / jmax(1L, count));
    CHECK(peak > 0.01);
    CHECK(peak < 4.0);
    CHECK(!player.isPlaying());

    // back to the built-in sounds: the plugin comes back to be deleted here
    auto old = player.setInstrument(nullptr);
    CHECK(old != nullptr);
    old.reset();

    // the mixer's Instrument strip: MIDI from the keyboard in, sound out, at the strip's level
    {
        TinCan::LiveInstrument live;
        live.prepare(rate, block);
        String err;
        auto li = formats.createPluginInstance(*desc, rate, block, err);
        CHECK(li != nullptr);
        live.setPlugin(std::move(li));
        MidiBuffer kb;
        double livePeak = 0.0;
        for (int b = 0; b < 60; ++b) {
            kb.clear();
            if (b == 2) kb.addEvent(MidiMessage::noteOn(1, 64, (uint8) 100), 100);
            if (b == 40) kb.addEvent(MidiMessage::noteOff(1, 64), 0);
            CHECK(live.render(kb, block));
            livePeak = jmax(livePeak, (double) live.getOutput().getMagnitude(0, 0, block));
        }
        CHECK(livePeak > 0.01);
        CHECK(live.meter.load() >= 0.0f);
        // muted: silent
        live.muted = true;
        kb.clear(); kb.addEvent(MidiMessage::noteOn(1, 67, (uint8) 100), 0);
        live.render(kb, block);   // the ramp down
        live.render(kb, block);
        CHECK(live.getOutput().getMagnitude(0, 0, block) < 1e-6f);
        CHECK(live.setPlugin(nullptr) != nullptr);
        CHECK(!live.render(kb, block));
        std::printf("  live instrument strip: peak %.2f\n", livePeak);
    }
    old = nullptr;
    std::printf("  vst3 instrument (%s): %s (peak %.2f, rms %.3f)\n", desc->name.toRawUTF8(),
                failures == before ? "ok" : "FAILED", peak, rms);
    old.reset();
#else
    std::printf("  vst3 instrument: skipped (no VST3 hosting in this build)\n");
#endif
    return failures;
}
