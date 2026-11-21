// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanLiveInstrument.h"

namespace TinCan {

static int channelsFor(AudioPluginInstance & p)
{
    return jmax(2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
}

void LiveInstrument::prepare(double sampleRate, int maxBlock)
{
    const SpinLock::ScopedLockType sl(lock);
    rate = sampleRate > 0 ? sampleRate : 48000.0;
    block = jmax(64, maxBlock);
    if (plugin) {
        plugin->releaseResources();
        plugin->setRateAndBufferSizeDetails(rate, block);
        plugin->prepareToPlay(rate, block);
    }
    work.setSize(plugin ? channelsFor(*plugin) : 2, block);
    output.setSize(2, block);
    midi.ensureSize(4096);
}

std::unique_ptr<AudioPluginInstance> LiveInstrument::setPlugin(std::unique_ptr<AudioPluginInstance> next)
{
    double r;
    int b;
    {
        const SpinLock::ScopedLockType sl(lock);
        r = rate;
        b = block;
    }
    AudioBuffer<float> w(next ? channelsFor(*next) : 2, b), o(2, b);
    if (next) {
        next->setRateAndBufferSizeDetails(r, b);
        next->prepareToPlay(r, b);
    }
    {
        const SpinLock::ScopedLockType sl(lock);
        std::swap(plugin, next);
        std::swap(work, w);
        std::swap(output, o);
        midi.clear();
        midi.ensureSize(4096);
    }
    meter = 0.0f;
    if (next) next->releaseResources();
    return next;
}

bool LiveInstrument::render(const MidiBuffer & midiIn, int numSamples)
{
    const SpinLock::ScopedTryLockType sl(lock);
    if (!sl.isLocked() || !plugin || numSamples <= 0 || numSamples > work.getNumSamples() || numSamples > output.getNumSamples()) {
        meter = 0.0f;
        return false;
    }
    midi.clear();
    for (const auto m : midiIn) midi.addEvent(m.data, m.numBytes, m.samplePosition);

    AudioBuffer<float> view(work.getArrayOfWritePointers(), work.getNumChannels(), numSamples);
    view.clear();
    plugin->processBlock(view, midi);

    const int outs = jmax(1, jmin(2, plugin->getTotalNumOutputChannels()));
    const float g = muted.load() ? 0.0f : jlimit(0.0f, 4.0f, gain.load());
    for (int c = 0; c < 2; ++c) output.copyFromWithRamp(c, 0, view.getReadPointer(jmin(c, outs - 1)), numSamples, lastGain, g);
    lastGain = g;

    const float rms = jmax(output.getRMSLevel(0, 0, numSamples), output.getRMSLevel(1, 0, numSamples));
    meter = jlimit(0.0f, 1.0f, (Decibels::gainToDecibels(rms, -60.0f) + 60.0f) / 60.0f);
    return true;
}

}

namespace TinCan {

void MidiRouter::prepare(double sampleRate)
{
    for (auto & c : collectors) c.reset(sampleRate > 0 ? sampleRate : 48000.0);
    prepared = true;
}

void MidiRouter::setDevice(int track, const String & identifier)
{
    if (track < 0 || track >= maxTracks) return;
    const SpinLock::ScopedLockType sl(lock);
    ids[track] = identifier;
}

String MidiRouter::getDevice(int track) const
{
    if (track < 0 || track >= maxTracks) return {};
    const SpinLock::ScopedLockType sl(lock);
    return ids[track];
}

void MidiRouter::handleIncomingMidiMessage(MidiInput * source, const MidiMessage & message)
{
    route(source != nullptr ? source->getIdentifier() : String(), message);
}

void MidiRouter::route(const String & from, const MidiMessage & message)
{
    if (!prepared.load()) return;
    bool want[maxTracks];
    {
        const SpinLock::ScopedLockType sl(lock);
        for (int k = 0; k < maxTracks; ++k) want[k] = ids[k].isEmpty() || ids[k] == from;
    }
    for (int k = 0; k < maxTracks; ++k)
        if (want[k]) collectors[k].addMessageToQueue(message);
}

void MidiRouter::pull(int track, MidiBuffer & out, int numSamples)
{
    out.clear();
    if (!prepared.load() || track < 0 || track >= maxTracks || numSamples <= 0) return;
    collectors[track].removeNextBlockOfMessages(out, numSamples);
}

}
