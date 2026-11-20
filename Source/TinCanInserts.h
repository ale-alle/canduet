// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "ChannelGroup.h"

#include <atomic>
#include <memory>

// VST3 effects on an input strip (T-086, stage B): two slots after the built-in gate, compressor and
// EQ, before the Reverb and Delay sends, so everyone hears them and they're recorded. A mono input
// goes into the plugin on both sides and comes back as the average of its two outputs.
namespace TinCan {

class InsertChain : public SonoAudio::InsertHook
{
public:
    static constexpr int numSlots = 2;

    // not the audio thread: the device's rate and largest block
    void prepare(double sampleRate, int maxBlock);

    // message thread: a plugin into a slot (nullptr empties it); the old one comes back to be deleted
    std::unique_ptr<AudioPluginInstance> setSlot(int slot, std::unique_ptr<AudioPluginInstance> plugin);
    AudioPluginInstance * getSlot(int slot) const;
    void setBypass(int slot, bool bypassed);
    bool getBypass(int slot) const;
    bool isEmpty() const;

    // audio thread: the input's channels [start, start + count) of buffer, in place
    void process(AudioBuffer<float> & buffer, int start, int count, int numSamples) override;

private:
    mutable SpinLock lock;
    std::unique_ptr<AudioPluginInstance> slots[numSlots];
    std::atomic<bool> bypass[numSlots] { { false }, { false } };
    AudioBuffer<float> scratch;
    MidiBuffer midi;
    double rate = 48000.0;
    int block = 512;
};

}
