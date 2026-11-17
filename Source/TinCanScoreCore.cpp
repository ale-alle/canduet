// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanScoreCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace TinCan {

void ScoreSchedule::setNotes(std::vector<ScoreNote> in, double end)
{
    notes.clear();
    for (const auto & n : in) {
        if (!std::isfinite(n.beat) || !std::isfinite(n.len) || n.beat < 0.0 || n.len <= 0.0 || n.midi < 0 || n.midi > 127) continue;
        notes.push_back(n);
    }
    std::stable_sort(notes.begin(), notes.end(), [](const ScoreNote & a, const ScoreNote & b) { return a.beat < b.beat; });
    double last = 0.0;
    for (const auto & n : notes) last = std::max(last, n.beat + n.len);
    endBeat = std::isfinite(end) ? std::max(end, last) : last;
    held.clear();
    held.reserve(notes.size() + 1);
    next = 0;
}

void ScoreSchedule::rewind()
{
    held.clear();
    next = 0;
}

int ScoreSchedule::process(double b0, double b1, int nframes, Event * out, int max)
{
    if (nframes <= 0 || b1 <= b0 || max <= 0) return 0;
    const double samplesPerBeat = nframes / (b1 - b0);
    // A beat belongs to the block holding its nearest sample: beats in [b0 - half, b1 - half), as
    // the metronome's clicks do (TinCanBeatClock.cpp). Blocks share that boundary, so rounding in
    // the block positions never moves a note into the block before.
    const double half = 0.5 / samplesPerBeat;
    const double from = b0 - half, to = b1 - half;
    auto offsetOf = [&](double beat) {
        int o = (int) std::floor((beat - from) * samplesPerBeat);
        return std::min(std::max(o, 0), nframes - 1);
    };
    int count = 0;

    // walk through the block in time order: the earliest pending off or on comes next
    while (count < max) {
        // earliest note-off inside the block
        int offIdx = -1;
        for (size_t i = 0; i < held.size(); ++i) {
            if (held[i].end < to && (offIdx < 0 || held[i].end < held[(size_t) offIdx].end)) offIdx = (int) i;
        }
        bool onReady = next < notes.size() && notes[next].beat < to;
        if (offIdx < 0 && !onReady) break;
        if (offIdx >= 0 && (!onReady || held[(size_t) offIdx].end <= notes[next].beat)) {
            out[count++] = { offsetOf(held[(size_t) offIdx].end), held[(size_t) offIdx].midi, false };
            held.erase(held.begin() + offIdx);
            continue;
        }
        const ScoreNote & n = notes[next++];
        if (n.beat + n.len <= from) continue;   // wholly in the past (playback started later)
        // the same key still sounding: end it first
        for (size_t i = 0; i < held.size(); ++i) {
            if (held[i].midi == n.midi) {
                if (count < max) out[count++] = { offsetOf(std::max(n.beat, from)), n.midi, false };
                held.erase(held.begin() + (long) i);
                break;
            }
        }
        if (count >= max) break;
        out[count++] = { offsetOf(std::max(n.beat, from)), n.midi, true };
        if (held.size() < held.capacity()) held.push_back({ n.beat + n.len, n.midi });
    }
    return count;
}

int ScoreSchedule::releaseAll(Event * out, int max)
{
    int count = 0;
    for (const auto & h : held) {
        if (count >= max) break;
        out[count++] = { 0, h.midi, false };
    }
    held.clear();
    return count;
}

double scoreStartOnMetronome(double metronomeBeat, double countInRemaining, int beatsPerBar)
{
    if (countInRemaining > 1e-9) return metronomeBeat + countInRemaining;
    if (beatsPerBar < 1) beatsPerBar = 1;
    // the next bar line; one that's only a few hundredths of a beat behind still counts as "now"
    double bars = metronomeBeat / beatsPerBar;
    double startBar = std::ceil(bars - 0.02);
    return std::max(startBar * beatsPerBar, metronomeBeat - 0.02 * beatsPerBar);
}

namespace TempoWire {

std::string encode(double bpm, int64_t seq)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"bpm\":%.2f,\"seq\":%lld}", bpm, (long long) seq);
    return buf;
}

// a tiny parser for exactly {"bpm":<number>,"seq":<integer>} (either order, spaces allowed)
bool decode(const std::string & s, double & bpm, int64_t & seq)
{
    if (s.size() > 120) return false;
    auto numberAfter = [&](const char * key, double & v) {
        auto k = s.find(key);
        if (k == std::string::npos) return false;
        k = s.find(':', k);
        if (k == std::string::npos) return false;
        const char * start = s.c_str() + k + 1;
        char * end = nullptr;
        v = std::strtod(start, &end);
        return end != start && std::isfinite(v);
    };
    double b = 0, q = 0;
    if (s.empty() || s.front() != '{' || s.back() != '}') return false;
    if (!numberAfter("\"bpm\"", b) || !numberAfter("\"seq\"", q)) return false;
    if (b < 10.0 || b > 400.0) return false;
    if (q < 1.0 || q > 1125899906842624.0 || q != std::floor(q)) return false;
    bpm = b;
    seq = (int64_t) q;
    return true;
}

bool newer(int64_t s1, const std::string & by1, int64_t s2, const std::string & by2)
{
    if (s1 != s2) return s1 > s2;
    return by1 > by2;
}

}

}
