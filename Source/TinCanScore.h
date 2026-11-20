// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "TinCanNoteShare.h"
#include "TinCanDock.h"
#include "TinCanScoreCore.h"

class SonobusAudioProcessor;

// The shared score (T-080): one staff that everyone in the lesson writes in together, played on each
// computer at the metronome's tempo. The page (score-page/, built into the app) does the music; this
// window carries its messages to the others in the lesson, plays it through ScorePlayer and hands it
// this computer's MIDI notes. See doc/TINCAN-SCORE.md.
namespace TinCan {

// Tag of the score's peer messages; the page keeps each one under 3000 bytes
constexpr const char * kScoreTag = "canduet-score-1";
constexpr int kScoreMaxPayload = 3000;

class ScoreWindow : public DockableWindow, private Timer, private NoteShare::Listener
{
public:
    struct Hooks
    {
        std::function<void(const String & payload)> sendToPeers;   // to everyone in the lesson
        std::function<StringArray()> peerNames;                    // who's in the lesson now
        std::function<String()> myName;
        bool practice = false;
    };

    ScoreWindow(SonobusAudioProcessor & processor, NoteShare & noteShare, Hooks hooks);
    ~ScoreWindow() override;

    // closing only hides it: the score (and the sharing) carry on until the lesson ends; in a lesson
    // it can dock inside the lesson window (T-085)

    // a score message from someone in the lesson (message thread)
    void receive(const String & from, const String & payload);

    // false when WebView2 can't run here (the window then just says so)
    bool hasPage() const { return browser != nullptr; }

private:
    class Browser;

    void timerCallback() override;
    void notesArrived(const std::vector<NoteEvent> & events) override;
    void handleMessage(const var & message);
    void post(const var & message);
    void play(const var & message);
    void record(const var & message);
    void stopPlaying();
    void setIntParam(const String & id, int value);
    void setMetronomeOn(bool on);
    bool isMetronomeOn() const;
    double getTempo() const;

    SonobusAudioProcessor & processor;
    NoteShare & noteShare;
    Hooks hooks;
    Browser * browser = nullptr;   // owned by the window
    bool ready = false;
    bool turnedMetronomeOn = false;   // switch it off again when playback stops
    int savedCountIn = -1;            // the metronome's count-in before recording changed it
    bool wasPlaying = false;
    double lastTempo = -1.0;
    StringArray lastPeers;
    int ticks = 0;
    std::vector<std::pair<String, String>> early;   // peer messages that came before the page was ready

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScoreWindow)
};

// Shared tempo (T-080): when anyone changes the metronome's tempo in a lesson, everyone's follows.
// Watches the tempo parameter; the newest change wins (TempoWire in TinCanScoreCore.h).
class TempoShare : private Timer
{
public:
    TempoShare(SonobusAudioProcessor & processor, std::function<void(const String & payload)> sendToPeers,
               std::function<bool()> inLesson, std::function<String()> myName);
    ~TempoShare() override;

    void receive(const String & from, const String & payload);
    void lessonEnded();

private:
    void timerCallback() override;
    double getTempo() const;

    SonobusAudioProcessor & processor;
    std::function<void(const String &)> sendToPeers;
    std::function<bool()> inLesson;
    std::function<String()> myName;
    double lastSeen = -1.0;
    int64 seq = 0;          // counter of the tempo we have now (0: never changed in this lesson)
    String seqBy;           // who made it
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoShare)
};

}
