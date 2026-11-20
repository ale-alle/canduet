// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// VST3 effect slots on an input (T-086). Part of TinCanAudioTests. Uses a stand-in plugin (half the
// level, and it swaps left and right), so it runs without any plugins on the computer.

#include <JuceHeader.h>
#include "../Source/TinCanInserts.h"

#include <cstdio>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace {
class HalfSwap : public AudioPluginInstance
{
public:
    HalfSwap() : AudioPluginInstance(BusesProperties().withInput("In", AudioChannelSet::stereo()).withOutput("Out", AudioChannelSet::stereo())) {}
    const String getName() const override { return "HalfSwap"; }
    void fillInPluginDescription(PluginDescription & d) const override { d.name = getName(); d.pluginFormatName = "Test"; }
    void prepareToPlay(double, int) override { prepared = true; }
    void releaseResources() override {}
    void processBlock(AudioBuffer<float> & b, MidiBuffer &) override
    {
        for (int i = 0; i < b.getNumSamples(); ++i) {
            const float l = b.getSample(0, i), r = b.getSample(1, i);
            b.setSample(0, i, r * 0.5f);
            b.setSample(1, i, l * 0.5f);
        }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    AudioProcessorEditor * createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const String getProgramName(int) override { return {}; }
    void changeProgramName(int, const String &) override {}
    void getStateInformation(MemoryBlock &) override {}
    void setStateInformation(const void *, int) override {}
    bool prepared = false;
};
}

int runInsertsTests()
{
    const int before = failures;
    TinCan::InsertChain chain;
    chain.prepare(48000.0, 256);
    CHECK(chain.isEmpty());

    AudioBuffer<float> buf(4, 256);
    auto fill = [&]() { for (int i = 0; i < 256; ++i) { buf.setSample(0, i, 0.8f); buf.setSample(1, i, 0.2f); buf.setSample(2, i, 0.6f); buf.setSample(3, i, 0.4f); } };

    // empty: untouched
    fill();
    chain.process(buf, 0, 2, 256);
    CHECK(buf.getSample(0, 10) == 0.8f && buf.getSample(1, 10) == 0.2f);

    auto p = std::make_unique<HalfSwap>();
    auto * raw = p.get();
    CHECK(chain.setSlot(0, std::move(p)) == nullptr);
    CHECK(raw->prepared && chain.getSlot(0) == raw && !chain.isEmpty());

    // a pair (channels 2-3): swapped and halved, the other channels left alone
    fill();
    chain.process(buf, 2, 2, 256);
    CHECK(std::abs(buf.getSample(2, 5) - 0.2f) < 1e-6f && std::abs(buf.getSample(3, 5) - 0.3f) < 1e-6f);
    CHECK(buf.getSample(0, 5) == 0.8f && buf.getSample(1, 5) == 0.2f);

    // a mono input (channel 0): in on both sides, back as the average: (0.4 + 0.4) / 2
    fill();
    chain.process(buf, 0, 1, 256);
    CHECK(std::abs(buf.getSample(0, 5) - 0.4f) < 1e-6f && buf.getSample(1, 5) == 0.2f);

    // two in a row: the second slot too
    chain.setSlot(1, std::make_unique<HalfSwap>());
    fill();
    chain.process(buf, 2, 2, 256);
    CHECK(std::abs(buf.getSample(2, 5) - 0.15f) < 1e-6f && std::abs(buf.getSample(3, 5) - 0.1f) < 1e-6f);

    // bypass skips one
    chain.setBypass(1, true);
    fill();
    chain.process(buf, 2, 2, 256);
    CHECK(std::abs(buf.getSample(2, 5) - 0.2f) < 1e-6f);

    // a block longer than prepared: left dry, never overruns
    AudioBuffer<float> big(2, 1024);
    big.clear(); big.setSample(0, 0, 1.0f);
    chain.process(big, 0, 2, 1024);
    CHECK(big.getSample(0, 0) == 1.0f);

    // emptying gives them back
    CHECK(chain.setSlot(0, nullptr) != nullptr);
    CHECK(chain.setSlot(1, nullptr) != nullptr);
    CHECK(chain.isEmpty());

    std::printf("  vst3 effect slots: %s\n", failures == before ? "ok" : "FAILED");
    return failures;
}
