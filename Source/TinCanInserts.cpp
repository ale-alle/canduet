// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanInserts.h"

namespace TinCan {

static int channelsFor(AudioPluginInstance & p)
{
    return jmax(2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
}

void InsertChain::prepare(double sampleRate, int maxBlock)
{
    const SpinLock::ScopedLockType sl(lock);
    rate = sampleRate > 0 ? sampleRate : 48000.0;
    block = jmax(64, maxBlock);
    int chans = 2;
    for (auto & p : slots) {
        if (!p) continue;
        p->releaseResources();
        p->setRateAndBufferSizeDetails(rate, block);
        p->prepareToPlay(rate, block);
        chans = jmax(chans, channelsFor(*p));
    }
    scratch.setSize(chans, block);
    midi.ensureSize(256);
}

std::unique_ptr<AudioPluginInstance> InsertChain::setSlot(int slot, std::unique_ptr<AudioPluginInstance> plugin)
{
    if (slot < 0 || slot >= numSlots) return plugin;
    double r;
    int b;
    {
        const SpinLock::ScopedLockType sl(lock);
        r = rate;
        b = block;
    }
    AudioBuffer<float> buffer;
    int chans = 2;
    if (plugin) {
        plugin->setRateAndBufferSizeDetails(r, b);
        plugin->prepareToPlay(r, b);
        chans = channelsFor(*plugin);
    }
    {
        const SpinLock::ScopedLockType sl(lock);
        for (int k = 0; k < numSlots; ++k) if (k != slot && slots[k]) chans = jmax(chans, channelsFor(*slots[k]));
        if (chans > scratch.getNumChannels() || scratch.getNumSamples() < b) buffer.setSize(chans, b);
        std::swap(slots[slot], plugin);
        if (buffer.getNumChannels() > 0) std::swap(scratch, buffer);
        bypass[slot] = false;
    }
    if (plugin) plugin->releaseResources();
    return plugin;
}

AudioPluginInstance * InsertChain::getSlot(int slot) const
{
    return slot >= 0 && slot < numSlots ? slots[slot].get() : nullptr;
}

void InsertChain::setBypass(int slot, bool b)
{
    if (slot >= 0 && slot < numSlots) bypass[slot] = b;
}

bool InsertChain::getBypass(int slot) const
{
    return slot >= 0 && slot < numSlots && bypass[slot].load();
}

bool InsertChain::isEmpty() const
{
    for (auto & p : slots) if (p) return false;
    return true;
}

void InsertChain::process(AudioBuffer<float> & buffer, int start, int count, int numSamples)
{
    if (count < 1 || numSamples <= 0) return;
    const SpinLock::ScopedTryLockType sl(lock);
    if (!sl.isLocked() || numSamples > scratch.getNumSamples()) return;   // a slot is changing: dry this block

    for (int s = 0; s < numSlots; ++s) {
        auto * p = slots[s].get();
        if (p == nullptr || bypass[s].load()) continue;
        const int chans = channelsFor(*p);
        AudioBuffer<float> view(scratch.getArrayOfWritePointers(), chans, numSamples);
        view.clear();
        // in: a mono input on both sides, a pair as it is
        view.copyFrom(0, 0, buffer, start, 0, numSamples);
        view.copyFrom(1, 0, buffer, count > 1 ? start + 1 : start, 0, numSamples);
        midi.clear();
        p->processBlock(view, midi);
        // out: the pair back, or the average for a mono input
        if (count > 1) {
            buffer.copyFrom(start, 0, view, 0, 0, numSamples);
            buffer.copyFrom(start + 1, 0, view, 1, 0, numSamples);
        }
        else {
            buffer.copyFrom(start, 0, view.getReadPointer(0), numSamples, 0.5f);
            buffer.addFrom(start, 0, view, 1, 0, numSamples, 0.5f);
        }
    }
}

}
