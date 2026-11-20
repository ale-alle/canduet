// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "TinCanScoreCore.h"

#include <atomic>

struct tsf;

// Plays the shared score (T-080) on this computer only: the notes go to this computer's output,
// never to the others in the lesson (owner decision 2026-11-17, each computer plays its own sound).
// The sound is a SoundFont (GeneralUser GS, sounds\ next to CanDuet.exe) played by TinySoundFont, or a
// VST3 instrument chosen in the Score window (T-086), which then plays every part instead.
// Playback follows the metronome: it starts after the count-in, or at the next bar if the metronome
// is already going, and keeps the metronome's tempo, so the clicks and the notes stay together.
namespace TinCan {

class ScorePlayer
{
public:
    ScorePlayer();
    ~ScorePlayer();

    // message thread ---------------------------------------------------------

    // loads the SoundFont the first time; false (with getError()) if it's missing or broken
    bool load();
    String getError() const { return error; }

    // program: General MIDI number (0 piano, 25 steel guitar, 33 bass). followMetronome: start with
    // the metronome (it must be switched on by the caller); otherwise start straight away.
    void play(std::vector<ScoreNote> notes, double endBeat, int program, bool followMetronome);
    void stop();

    // score position in beats (negative during the count-in), and whether it's still going
    double getPosition() const { return position.load(); }
    bool isPlaying() const { return playing.load(); }

    // audio thread -----------------------------------------------------------

    void prepare(double sampleRate, int maxBlock);

    // Adds the score's sound to the first two channels of out. Metronome state at the start of
    // this block: whether it's running on its own clock, its beat count and count-in left.
    void process(AudioBuffer<float> & out, int numChannels, int numSamples, double tempo,
                 bool metronomeRunning, double metronomeBeat, double countInRemaining, int beatsPerBar);

    // the score beat at a sample of the coming block (for stamping MIDI notes while recording);
    // -1e9 when nothing is playing or the start isn't known yet
    double beatAt(int sampleOffset, double metronomeBeat, double tempo, bool metronomeRunning) const;

    // message thread (T-086): a VST3 instrument instead of the SoundFont (nullptr: back to the
    // SoundFont). The previous one is handed back to be deleted on the message thread.
    std::unique_ptr<AudioPluginInstance> setInstrument(std::unique_ptr<AudioPluginInstance> instrument);
    AudioPluginInstance * getInstrument() const { return instrument.get(); }

private:
    void renderTo(float * L, float * R, int from, int count);
    void noteOn(int midi, int offset);
    void noteOff(int midi, int offset);
    void renderPlugin(AudioBuffer<float> & out, int numChannels, int numSamples);

    tsf * synth = nullptr;
    String error;
    bool loaded = false;

    // handed from the message thread to the audio thread under the lock (the audio thread only tries it)
    SpinLock lock;
    struct Request
    {
        bool pending = false;
        bool stop = false;
        ScoreSchedule schedule;
        int preset = 0;
        bool follow = false;
    } request;

    // audio thread state
    ScoreSchedule schedule;
    int preset = 0;
    bool active = false;
    bool follow = false;
    bool anchored = false;
    double anchor = 0.0;      // metronome beat where score beat 0 falls
    double ownBeat = 0.0;     // score beat when not following the metronome
    int waitBlocks = 0;       // blocks waited for the metronome to start
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    std::vector<float> interleaved;

    // the VST3 instrument (T-086): swapped under the lock; played while notes sound and a while after
    std::unique_ptr<AudioPluginInstance> instrument;
    MidiBuffer midi;
    AudioBuffer<float> pluginBuffer;
    int64 tailSamples = 0;

    std::atomic<double> position { 0.0 };
    std::atomic<bool> playing { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScorePlayer)
};

// where the SoundFont is: TINCAN_SOUNDFONT, else sounds\GeneralUser-GS.sf2 next to the exe
File getScoreSoundFontFile();

}
