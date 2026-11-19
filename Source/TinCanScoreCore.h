// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The parts of the shared score (T-080) that don't need JUCE, so they're unit tested in
// tests/TinCanScoreCoreTest.cpp: the playback schedule, where playback starts against the
// metronome, and the tempo messages everyone in a lesson shares.
namespace TinCan {

struct ScoreNote
{
    double beat = 0.0;   // quarter notes from the start
    double len = 0.0;
    int midi = 60;
};

// Note-ons and note-offs for one stretch of time. Runs on the audio thread: no allocation after
// setNotes (which reserves room for every note to be held at once).
class ScoreSchedule
{
public:
    struct Event
    {
        int offset = 0;   // sample within the block
        int midi = 0;
        bool on = false;
    };

    // message thread, before handing the schedule to the audio thread; sorts and checks the notes
    void setNotes(std::vector<ScoreNote> notes, double endBeat);

    void rewind();

    // Events for beats [b0, b1) of a block of nframes samples, in time order (offs before ons at the
    // same sample, so a repeated note restarts). Returns how many were written (at most max).
    int process(double b0, double b1, int nframes, Event * out, int max);

    // offs for every sounding note, at offset 0; returns how many were written
    int releaseAll(Event * out, int max);

    bool finished(double beat) const { return beat >= endBeat && held.empty(); }
    double getEndBeat() const { return endBeat; }
    size_t size() const { return notes.size(); }

private:
    struct Held { double end; int midi; };
    std::vector<ScoreNote> notes;
    std::vector<Held> held;
    size_t next = 0;
    double endBeat = 0.0;
};

// Where score beat 0 falls on the metronome's beat count (its position counts the count-in from 0):
// straight after the count-in if one is running, otherwise at the next bar line.
double scoreStartOnMetronome(double metronomeBeat, double countInRemaining, int beatsPerBar);

// Shared tempo (T-080): anyone in the lesson can change the metronome's tempo. Each change carries
// a counter; the highest counter wins and ties go to the higher name, so everyone ends on the same
// tempo even if two people change it at once.
namespace TempoWire {
    constexpr const char * Tag = "canduet-tempo-1";
    std::string encode(double bpm, int64_t seq);
    // false if the payload isn't a tempo message in range (10-400 bpm, counter 1..2^50)
    bool decode(const std::string & payload, double & bpm, int64_t & seq);
    bool newer(int64_t seq, const std::string & by, int64_t seqThan, const std::string & byThan);
}

}
