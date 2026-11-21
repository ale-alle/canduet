// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

#include <atomic>
#include <memory>

// The mixer's Instrument strip (T-086, stage C): a VST3 instrument played by this computer's MIDI
// keyboard. Its sound goes to the others in the lesson, to what you hear (whatever "Hear myself" is
// set to, since a keyboard makes no sound of its own) and into recordings.
namespace TinCan {

class LiveInstrument
{
public:
    // not the audio thread
    void prepare(double sampleRate, int maxBlock);

    // message thread: the instrument (nullptr: none); the old one comes back to be deleted there
    std::unique_ptr<AudioPluginInstance> setPlugin(std::unique_ptr<AudioPluginInstance> plugin);
    AudioPluginInstance * getPlugin() const { return plugin.get(); }

    std::atomic<float> gain { 1.0f };      // 0..4, the strip's fader
    std::atomic<bool> muted { false };
    std::atomic<float> meter { 0.0f };     // the last block's level, 0..1 over -60..0 dB

    // audio thread: plays this block's MIDI; true when there's sound in getOutput() (stereo)
    bool render(const MidiBuffer & midiIn, int numSamples);
    const AudioBuffer<float> & getOutput() const { return output; }

private:
    SpinLock lock;
    std::unique_ptr<AudioPluginInstance> plugin;
    AudioBuffer<float> work, output;
    MidiBuffer midi;
    double rate = 48000.0;
    int block = 512;
    float lastGain = 1.0f;
};

}

// T-087: up to four Instrument strips, each played by all MIDI keyboards or one. The app's
// MIDI comes to the processor as one merged stream, so this listens to every enabled MIDI
// device itself (with the device it came from) and hands each strip only its own keyboard's notes.
namespace TinCan {

class MidiRouter : public MidiInputCallback
{
public:
    static constexpr int maxTracks = 4;

    // not the audio thread
    void prepare(double sampleRate);

    // message thread: the strip's device identifier, empty = all devices
    void setDevice(int track, const String & identifier);
    String getDevice(int track) const;

    // MIDI thread(s)
    void handleIncomingMidiMessage(MidiInput * source, const MidiMessage & message) override;
    void route(const String & fromDevice, const MidiMessage & message);   // what that does (tests call it)

    // audio thread: this block's notes for a strip
    void pull(int track, MidiBuffer & out, int numSamples);

    std::atomic<bool> attached { false };   // listening to the devices (the app, not a plugin build)

private:
    mutable SpinLock lock;
    String ids[maxTracks];
    MidiMessageCollector collectors[maxTracks];
    std::atomic<bool> prepared { false };
};

}
