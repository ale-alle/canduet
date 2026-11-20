// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// The score played by a VST3 instrument (T-086). Part of TinCanAudioTests. Needs a VST3 instrument on
// this computer: TINCAN_TEST_VST3 (path to a .vst3), else Surge XT where its installer puts it;
// skipped (not failed) when there's none.

#include <JuceHeader.h>
#include "../Source/TinCanScorePlayer.h"

#include <cstdio>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int runVstTests()
{
    const int before = failures;
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
    std::printf("  vst3 instrument (%s): %s (peak %.2f, rms %.3f)\n", desc->name.toRawUTF8(),
                failures == before ? "ok" : "FAILED", peak, rms);
    old.reset();
#else
    std::printf("  vst3 instrument: skipped (no VST3 hosting in this build)\n");
#endif
    return failures;
}
