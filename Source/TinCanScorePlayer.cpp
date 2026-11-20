// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanScorePlayer.h"

#if JUCE_MSVC
 #pragma warning (push)
 #pragma warning (disable: 4244 4267 4456 4701 4996)
#endif
#define TSF_IMPLEMENTATION
#include "../deps/tsf/tsf.h"
#if JUCE_MSVC
 #pragma warning (pop)
#endif

namespace TinCan {

File getScoreSoundFontFile()
{
    auto dev = SystemStats::getEnvironmentVariable("TINCAN_SOUNDFONT", {}).trim();
    if (dev.isNotEmpty() && File::isAbsolutePath(dev)) return File(dev);
    return File::getSpecialLocation(File::currentExecutableFile).getParentDirectory()
        .getChildFile("sounds").getChildFile("GeneralUser-GS.sf2");
}

ScorePlayer::ScorePlayer() = default;

ScorePlayer::~ScorePlayer()
{
    // the processor stops calling process() before it's destroyed
    if (synth) tsf_close(synth);
}

bool ScorePlayer::load()
{
    if (loaded) return synth != nullptr;
    loaded = true;
    auto file = getScoreSoundFontFile();
    if (!file.existsAsFile()) {
        error = TRANS("The score's sounds are missing (") + file.getFullPathName() + "). " + TRANS("Reinstall CanDuet to get them back.");
        return false;
    }
    auto * f = tsf_load_filename(file.getFullPathName().toRawUTF8());
    if (!f) {
        error = TRANS("The score's sounds couldn't be read.");
        return false;
    }
    // voices are made now so the audio thread never allocates; extra notes take over the oldest
    tsf_set_max_voices(f, 64);   // T-077: plenty for a score, and a bounded cost on the audio thread
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, (int) sampleRate, -4.0f);
    {
        const SpinLock::ScopedLockType sl(lock);
        synth = f;
    }
    return true;
}

void ScorePlayer::play(std::vector<ScoreNote> notes, double endBeat, int program, bool followMetronome)
{
    // a VST3 instrument plays without the SoundFont (T-086)
    const bool sf = load();
    if (!sf && !instrument) return;
    int index = sf ? tsf_get_presetindex(synth, 0, jlimit(0, 127, program)) : 0;
    if (sf && index < 0) index = tsf_get_presetindex(synth, 0, 0);
    ScoreSchedule s;
    s.setNotes(std::move(notes), endBeat);
    {
        const SpinLock::ScopedLockType sl(lock);
        request.pending = true;
        request.stop = false;
        request.schedule = std::move(s);
        request.preset = jmax(0, index);
        request.follow = followMetronome;
    }
    playing = true;
    position = followMetronome ? -1.0e9 : 0.0;
}

void ScorePlayer::stop()
{
    {
        const SpinLock::ScopedLockType sl(lock);
        request.pending = true;
        request.stop = true;
    }
    playing = false;
}

void ScorePlayer::prepare(double rate, int maxBlock)
{
    const SpinLock::ScopedLockType sl(lock);
    sampleRate = rate > 0 ? rate : 48000.0;
    maxBlockSize = jmax(512, maxBlock);
    interleaved.assign((size_t) maxBlockSize * 2, 0.0f);
    midi.ensureSize(4096);
    if (instrument) {
        instrument->releaseResources();
        instrument->setRateAndBufferSizeDetails(sampleRate, maxBlockSize);
        instrument->prepareToPlay(sampleRate, maxBlockSize);
        pluginBuffer.setSize(jmax(2, instrument->getTotalNumInputChannels(), instrument->getTotalNumOutputChannels()), maxBlockSize);
    }
    if (synth) {
        tsf_set_output(synth, TSF_STEREO_INTERLEAVED, (int) sampleRate, -4.0f);
        tsf_reset(synth);
    }
}

double ScorePlayer::beatAt(int sampleOffset, double metronomeBeat, double tempo, bool metronomeRunning) const
{
    // audio thread only, like process(): these are its own variables
    if (!active) return -1.0e9;
    const double bps = (tempo > 0 ? tempo : 100.0) / (60.0 * sampleRate);
    if (follow) {
        if (!anchored || !metronomeRunning) return -1.0e9;
        return metronomeBeat - anchor + sampleOffset * bps;
    }
    return ownBeat + sampleOffset * bps;
}

std::unique_ptr<AudioPluginInstance> ScorePlayer::setInstrument(std::unique_ptr<AudioPluginInstance> next)
{
    double rate;
    int block;
    {
        const SpinLock::ScopedLockType sl(lock);
        rate = sampleRate;
        block = maxBlockSize;
    }
    AudioBuffer<float> buffer;
    if (next) {
        // ready before the audio thread sees it
        next->setRateAndBufferSizeDetails(rate, block);
        next->prepareToPlay(rate, block);
        buffer.setSize(jmax(2, next->getTotalNumInputChannels(), next->getTotalNumOutputChannels()), block);
    }
    {
        const SpinLock::ScopedLockType sl(lock);
        std::swap(instrument, next);
        std::swap(pluginBuffer, buffer);
        midi.clear();
        midi.ensureSize(4096);
        tailSamples = 0;
        if (active) {
            // the notes that were sounding stop; playback carries on with the new sound
            ScoreSchedule::Event events[256];
            schedule.releaseAll(events, 256);
        }
        if (synth) tsf_note_off_all(synth);
    }
    if (next) next->releaseResources();
    return next;
}

void ScorePlayer::noteOn(int note, int offset)
{
    if (instrument) { midi.addEvent(MidiMessage::noteOn(1, note, (uint8) 96), offset); tailSamples = (int64) (sampleRate * 10.0); }
    else tsf_note_on(synth, preset, note, 0.75f);
}

void ScorePlayer::noteOff(int note, int offset)
{
    if (instrument) midi.addEvent(MidiMessage::noteOff(1, note), offset);
    else tsf_note_off(synth, preset, note);
}

void ScorePlayer::renderPlugin(AudioBuffer<float> & out, int numChannels, int numSamples)
{
    // one call for the block, notes at their sample offsets; the plugin's first two outputs are added
    if (numSamples > pluginBuffer.getNumSamples()) { midi.clear(); return; }
    AudioBuffer<float> view(pluginBuffer.getArrayOfWritePointers(), pluginBuffer.getNumChannels(), numSamples);
    view.clear();
    instrument->processBlock(view, midi);
    midi.clear();
    const int outs = jmin(2, instrument->getTotalNumOutputChannels(), view.getNumChannels());
    for (int c = 0; c < numChannels && c < 2; ++c) {
        if (outs <= 0) break;
        out.addFrom(c, 0, view, jmin(c, outs - 1), 0, numSamples);
    }
    tailSamples = jmax((int64) 0, tailSamples - numSamples);
}

void ScorePlayer::renderTo(float * L, float * R, int from, int count)
{
    while (count > 0) {
        int n = jmin(count, (int) interleaved.size() / 2);
        tsf_render_float(synth, interleaved.data(), n, 0);
        for (int i = 0; i < n; ++i) {
            L[from + i] += interleaved[(size_t) (2 * i)];
            if (R) R[from + i] += interleaved[(size_t) (2 * i + 1)];
        }
        from += n;
        count -= n;
    }
}

void ScorePlayer::process(AudioBuffer<float> & out, int numChannels, int numSamples, double tempo,
                          bool metronomeRunning, double metronomeBeat, double countInRemaining, int beatsPerBar)
{
    const SpinLock::ScopedTryLockType sl(lock);
    if (!sl.isLocked() || (!synth && !instrument) || numChannels < 1 || numSamples <= 0) return;

    ScoreSchedule::Event events[256];

    if (request.pending) {
        request.pending = false;
        int n = schedule.releaseAll(events, 256);
        for (int i = 0; i < n; ++i) noteOff(events[i].midi, 0);
        if (request.stop) {
            active = false;
        }
        else {
            std::swap(schedule, request.schedule);
            preset = request.preset;
            follow = request.follow;
            active = true;
            anchored = false;
            ownBeat = 0.0;
            waitBlocks = 0;
        }
    }

    float * L = out.getWritePointer(0);
    float * R = numChannels > 1 ? out.getWritePointer(1) : nullptr;

    if (!active) {
        // let released notes ring out
        if (instrument) { if (tailSamples > 0 || !midi.isEmpty()) renderPlugin(out, numChannels, numSamples); }
        else if (tsf_active_voice_count(synth) > 0) renderTo(L, R, 0, numSamples);
        return;
    }

    const double beatsPerSample = (tempo > 0 ? tempo : 100.0) / (60.0 * sampleRate);

    // where this block starts, in score beats
    double b0;
    if (follow && !anchored) {
        if (metronomeRunning) {
            anchor = scoreStartOnMetronome(metronomeBeat, countInRemaining, beatsPerBar);
            anchored = true;
        }
        else if (++waitBlocks * numSamples > sampleRate / 2) {
            follow = false;   // the metronome didn't start: play without it
        }
        else {
            position = -1.0e9;
            if (instrument) renderPlugin(out, numChannels, numSamples);
            else renderTo(L, R, 0, numSamples);
            return;
        }
    }
    if (follow && anchored && metronomeRunning) b0 = metronomeBeat - anchor;
    else b0 = ownBeat;
    // if the metronome stops mid-way, carry on from here on our own
    if (follow && anchored && !metronomeRunning) follow = false;

    const double b1 = b0 + numSamples * beatsPerSample;
    ownBeat = b1;
    position = b0;

    int n = schedule.process(b0, b1, numSamples, events, 256);
    if (instrument) {
        for (int i = 0; i < n; ++i) {
            if (events[i].on) noteOn(events[i].midi, events[i].offset);
            else noteOff(events[i].midi, events[i].offset);
        }
        renderPlugin(out, numChannels, numSamples);
    }
    else {
        int pos = 0;
        for (int i = 0; i < n; ++i) {
            if (events[i].offset > pos) { renderTo(L, R, pos, events[i].offset - pos); pos = events[i].offset; }
            if (events[i].on) noteOn(events[i].midi, events[i].offset);
            else noteOff(events[i].midi, events[i].offset);
        }
        if (pos < numSamples) renderTo(L, R, pos, numSamples - pos);
    }

    if (schedule.finished(b1)) {
        active = false;
        playing = false;
    }
}

}
