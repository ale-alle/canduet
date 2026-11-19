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
    if (!load()) return;
    int index = tsf_get_presetindex(synth, 0, jlimit(0, 127, program));
    if (index < 0) index = tsf_get_presetindex(synth, 0, 0);
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
    interleaved.assign((size_t) jmax(512, maxBlock) * 2, 0.0f);
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
    if (!sl.isLocked() || !synth || numChannels < 1 || numSamples <= 0) return;

    ScoreSchedule::Event events[256];

    if (request.pending) {
        request.pending = false;
        int n = schedule.releaseAll(events, 256);
        for (int i = 0; i < n; ++i) tsf_note_off(synth, preset, events[i].midi);
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
        if (tsf_active_voice_count(synth) > 0) renderTo(L, R, 0, numSamples);
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
            renderTo(L, R, 0, numSamples);
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
    int pos = 0;
    for (int i = 0; i < n; ++i) {
        if (events[i].offset > pos) { renderTo(L, R, pos, events[i].offset - pos); pos = events[i].offset; }
        if (events[i].on) tsf_note_on(synth, preset, events[i].midi, 0.75f);
        else tsf_note_off(synth, preset, events[i].midi);
    }
    if (pos < numSamples) renderTo(L, R, pos, numSamples - pos);

    if (schedule.finished(b1)) {
        active = false;
        playing = false;
    }
}

}
