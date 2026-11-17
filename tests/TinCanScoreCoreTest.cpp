// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanTest.h"
#include "../Source/TinCanScoreCore.h"

#include <cmath>
#include <vector>

using namespace TinCan;

namespace {

struct Hit { long sample; int midi; bool on; };

// plays the schedule at a fixed tempo in blocks, returning every event with its absolute sample
std::vector<Hit> runSchedule(ScoreSchedule & s, double startBeat, double bpm, double rate, int block, long total)
{
    std::vector<Hit> out;
    ScoreSchedule::Event ev[64];
    const double bps = bpm / 60.0 / rate;
    double beat = startBeat;
    for (long done = 0; done < total; done += block) {
        int n = (int) std::min<long>(block, total - done);
        int c = s.process(beat, beat + n * bps, n, ev, 64);
        for (int i = 0; i < c; ++i) out.push_back({ done + ev[i].offset, ev[i].midi, ev[i].on });
        beat += n * bps;
    }
    return out;
}

void testScheduleTiming()
{
    // 120 bpm at 48 kHz: one beat = 24000 samples
    ScoreSchedule s;
    s.setNotes({ { 0.0, 1.0, 60 }, { 1.0, 0.5, 62 }, { 1.5, 0.5, 64 }, { 0.0, 2.0, 48 } }, 4.0);
    auto hits = runSchedule(s, 0.0, 120.0, 48000.0, 512, 48000 * 3);
    // ons at 0 (two), 24000, 36000; offs at 24000 (60), 36000 (62), 48000 (64 and 48)
    int ons = 0, offs = 0;
    for (auto & h : hits) (h.on ? ons : offs)++;
    CHECK(ons == 4);
    CHECK(offs == 4);
    auto find = [&](int midi, bool on) { for (auto & h : hits) if (h.midi == midi && h.on == on) return h.sample; return -1L; };
    CHECK(find(60, true) == 0);
    CHECK(find(62, true) == 24000);
    CHECK(std::labs(find(60, false) - 24000) <= 1);
    CHECK(std::labs(find(64, true) - 36000) <= 1);
    CHECK(std::labs(find(48, false) - 48000) <= 1);
    // at 24000 the off for 60 comes before the on for 62
    size_t offAt = 0, onAt = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].midi == 60 && !hits[i].on) offAt = i;
        if (hits[i].midi == 62 && hits[i].on) onAt = i;
    }
    CHECK(offAt < onAt);
    CHECK(s.finished(4.0));
    CHECK(!s.finished(3.9));
}

void testScheduleSameBlockSizesAgree()
{
    // the same schedule in odd block sizes gives the same samples (within one)
    std::vector<ScoreNote> notes;
    for (int i = 0; i < 64; ++i) notes.push_back({ i * 0.25, 0.25, 60 + (i % 12) });
    ScoreSchedule a, b;
    a.setNotes(notes, 16.0);
    b.setNotes(notes, 16.0);
    auto ha = runSchedule(a, 0.0, 97.0, 44100.0, 64, 44100 * 11);
    auto hb = runSchedule(b, 0.0, 97.0, 44100.0, 1031, 44100 * 11);
    CHECK(ha.size() == hb.size());
    for (size_t i = 0; i < ha.size() && i < hb.size(); ++i) {
        CHECK(ha[i].midi == hb[i].midi && ha[i].on == hb[i].on);
        CHECK(std::labs(ha[i].sample - hb[i].sample) <= 1);
    }
}

void testScheduleRepeatedNoteAndCountIn()
{
    // count-in: playback starts at beat -4; nothing sounds before beat 0
    ScoreSchedule s;
    s.setNotes({ { 0.0, 1.0, 60 }, { 0.5, 1.0, 60 } }, 2.0);   // overlapping repeat of the same key
    auto hits = runSchedule(s, -4.0, 60.0, 1000.0, 100, 7000);
    CHECK(hits.size() == 4);
    if (hits.size() == 4) {
        CHECK(hits[0].on && hits[0].sample == 4000);
        CHECK(!hits[1].on && hits[1].sample == 4500);   // ended so it can restart
        CHECK(hits[2].on && hits[2].sample == 4500);
        CHECK(!hits[3].on && hits[3].sample == 5500);
    }
}

void testScheduleSkipsBadNotesAndReleases()
{
    ScoreSchedule s;
    s.setNotes({ { -1.0, 1.0, 60 }, { 0.0, 0.0, 61 }, { 0.0, 1.0, 200 }, { NAN, 1.0, 62 }, { 0.0, 8.0, 64 } }, 1.0);
    CHECK(s.size() == 1);
    CHECK(s.getEndBeat() == 8.0);
    ScoreSchedule::Event ev[8];
    int c = s.process(0.0, 0.1, 100, ev, 8);
    CHECK(c == 1 && ev[0].on && ev[0].midi == 64);
    c = s.releaseAll(ev, 8);
    CHECK(c == 1 && !ev[0].on && ev[0].midi == 64);
    CHECK(s.releaseAll(ev, 8) == 0);
}

void testStartOnMetronome()
{
    CHECK(scoreStartOnMetronome(0.0, 8.0, 4) == 8.0);         // fresh start with two bars to count in
    CHECK(scoreStartOnMetronome(2.5, 5.5, 4) == 8.0);         // count-in already running
    CHECK(scoreStartOnMetronome(9.3, 0.0, 4) == 12.0);        // running: the next bar
    CHECK(scoreStartOnMetronome(12.01, 0.0, 4) == 12.0);      // just past a bar line: now
    CHECK(scoreStartOnMetronome(0.0, 0.0, 3) == 0.0);
    CHECK(scoreStartOnMetronome(4.0, 0.0, 0) == 4.0);
}

void testTempoWire()
{
    double bpm = 0; int64_t seq = 0;
    CHECK(TempoWire::decode(TempoWire::encode(96.5, 3), bpm, seq));
    CHECK(bpm == 96.5 && seq == 3);
    CHECK(TempoWire::decode("{ \"seq\": 7, \"bpm\": 120 }", bpm, seq) && bpm == 120.0 && seq == 7);
    CHECK(!TempoWire::decode("", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":5,\"seq\":1}", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":500,\"seq\":1}", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":100,\"seq\":0}", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":100,\"seq\":1.5}", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":nan,\"seq\":1}", bpm, seq));
    CHECK(!TempoWire::decode("{\"bpm\":100}", bpm, seq));
    CHECK(!TempoWire::decode(std::string(200, ' '), bpm, seq));
    CHECK(TempoWire::newer(2, "Ann", 1, "Zed"));
    CHECK(TempoWire::newer(2, "Zed", 2, "Ann"));
    CHECK(!TempoWire::newer(2, "Ann", 2, "Zed"));
    CHECK(!TempoWire::newer(2, "Ann", 2, "Ann"));
}

}

void runScoreCoreTests()
{
    testScheduleTiming();
    testScheduleSameBlockSizesAgree();
    testScheduleRepeatedNoteAndCountIn();
    testScheduleSkipsBadNotesAndReleases();
    testStartOnMetronome();
    testTempoWire();
}
