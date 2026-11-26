// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "SonobusPluginProcessor.h"
#include "TinCanVideoTiles.h"
#include "TinCanTools.h"
#include "TinCanDock.h"
#include "TinCanHomework.h"

#include <functional>
#include <map>

// The lesson window (T-070, D-028).
//
// During a lesson one web page (video-page/lesson.html, served next to the video page) covers the
// whole main window: the people as video tiles with the red string between them, each person's sound
// on hover and behind the tile, the lesson bar and all its popovers. There are no SonoBus audio rows.
// Tools, Board and Score each open docked inside the lesson window or in a window of their own, as each
// was last left (T-085). One tool is docked at a time: Tools along the bottom, Board or Score in the right
// two thirds with the people small on the left. Tools in their own windows stay open whatever is docked.
//
// WebView2 is a native window of its own, so nothing JUCE draws inside the main window can appear on
// top of it. That is why everything that floats lives in the page, and why the dock is a borderless
// child window (created after the web view, so it sits above it) laid over a gap the page leaves.
//
// Messages (JSON, WebView2 postMessage):
//   app  -> page  {"t":"state","me":{…},"inputs":[{"i","label","name","stereo","canPair","gain","pan","muted","level","levels"}],"conn":"connecting|connected|lost","lesson":{…},
//                  "peers":[{…}],"rec":{"phase","secs"},"docks":{"tools","board","score","docked"}}   ~10 a second
//                  (docks: which tools are open, and which one is docked: "tools", "board", "score" or "")
//                 {"t":"chat","items":[{"from","text","me","sys"}]}                    new messages
//                 {"t":"toast","text","ms"}                                           a short notice
//   page -> app   {"t":"ready"}
//                 {"t":"gain"|"pan","id","v"}                    a person's volume (0..2) or pan (-1..1)
//                 {"t":"peer","id","k":"mute|solo|quality|buffer|resetBuffer|remove","v"}
//                 {"t":"mic","off"} {"t":"inGain","v"} {"t":"volume","v"} {"t":"monitor","v"} {"t":"monitorLevel","v"} {"t":"outMute","v"} {"t":"recvMute","v"}
//                 {"t":"input","i","k":"gain|pan|mute|pair","v"}   one input in the input mixer (T-077)
//                 {"t":"slot","i","s"}                          a strip's VST3 effect slot: its menu (T-086)
//                 {"t":"instrument","k":"gain|mute|menu","v"}   the Instrument strip (T-086); state has "instrument":{name,gain,muted,level}
//                 {"t":"tools"} {"t":"board"} {"t":"score"} {"t":"setup"} {"t":"settings"} {"t":"effects"} {"t":"help"} {"t":"leave"}
//                 {"t":"rec","a":"ask|stop"} {"t":"mark","a":"start|save","label"}
//                 {"t":"chat","text"}
//                 {"t":"dock","rect":[x,y,w,h],"side":[x,y,w,h]}  the gaps for docked Tools (bottom) and Board or Score
//                                                                (right), as fractions of the page ([] = none)
namespace TinCan {

// lesson page: TINCAN_LESSON_PAGE (https, or http to localhost), else <video page>lesson.html
String getLessonPage();

// Practice mode's page (T-075): the app's own copy next to CanDuet.exe (lesson\lesson.html), so it
// works without the internet; TINCAN_PRACTICE_PAGE (https, or http to localhost) for development.
// Empty when the copy is missing.
String getPracticePage();

struct LessonInfo
{
    String title;
    String myName;
    int64 startsAtMs = 0;   // 0 when the server didn't say
    int64 endsAtMs = 0;
    bool teacher = true;    // a missing role (older server) gets the full controls, as before
    bool practice = false;  // Practice mode (T-075): on your own, no lesson room, no video room
};

class LessonView : public Component, private Timer, private NoteShare::Listener
{
public:
    struct Hooks
    {
        std::function<void()> openSetup, openSettings, openEffects, openHelp, leave, shareVideoId;
        // the Board and Score windows, made when create is true (else nullptr when there isn't one)
        std::function<DockableWindow *(bool create)> boardWindow, scoreWindow;
        std::function<void()> recordAsk, recordStop, markStart;
        std::function<void(const String &)> markSave;
        std::function<String()> recordPhase;      // "idle", "asking", "asked" or "recording"
        std::function<double()> recordSeconds;
        std::function<void(String & in, String & out)> deviceNames;
        // the audio interface's inputs that are on, in order: device channel numbers (0-based) and names
        std::function<void(Array<int> & numbers, StringArray & names)> inputChannels;
        std::function<std::unique_ptr<ToolsPanel>(ToolsPanel::Set)> makeTools;
        // Homework & Recordings (T-090), for the lesson starting then (0: Practice mode)
        std::function<std::unique_ptr<HomeworkPanel>(int64 lessonStartMs)> makeHomework;
        NoteShare * noteShare = nullptr;   // MIDI notes go to the page too, for lesson modules (T-075)
    };

    // returns nullptr when WebView2 can't start on this computer
    static std::unique_ptr<LessonView> create(SonobusAudioProcessor & processor, const VideoInfo & video,
                                              const LessonInfo & info, Hooks hooks);
    ~LessonView() override;

    void resized() override;
    void paint(Graphics & g) override;

    // the other app's video identity, from its peer message (TinCanVideoTiles.h)
    void setPeerVideoId(const String & userName, const String & identity);

    void showToast(const String & text, int timeoutMs = 5000);

    bool isPractice() const { return info.practice; }
    // "tools", "metro", "tuner", "board" or "score": opened docked or in its own window, as it was last left
    void openTool(const String & id);
    void closeTool(const String & id);
    bool isToolOpen(const String & id);
    bool isToolsOpen() { return isToolOpen("tools"); }
    void setToolsOpen(bool open) { if (open) openTool("tools"); else closeTool("tools"); }
    ToolsPanel * getTools() const;
    HomeworkPanel * getHomework() const;

private:
    class DockFrame;
    class MixerWindow;
    // the tools the lesson window makes itself (the rest are the editor's Board and Score windows)
    static bool isOwnTool(const String & id) { return id == "tools" || id == "metro" || id == "tuner" || id == "homework"; }
    // where a docked tool goes: along the bottom (Tools, Metronome, Tuner) or at the right (the rest)
    static bool docksAtBottom(const String & id) { return id == "tools" || id == "metro" || id == "tuner"; }
    // owner 2026-11-25: the mixer in a window of its own (a second copy of the page, mixer only), or back in the page
    void setMixerOut(bool out);
    void showMixerWindow(bool show);

    LessonView(SonobusAudioProcessor & processor, std::unique_ptr<VideoTilesView> page, const LessonInfo & info, Hooks hooks);

    void timerCallback() override;
    void notesArrived(const std::vector<NoteEvent> & events) override;
    void handleMessage(const var & message);
    void handlePeerMessage(int index, const String & kind, const var & value);
    void sendState();
    var inputsState();
    var effectsState(int group);
    var busState();
    void sendNewChat();
    void placeFrame();
    DockableWindow * windowFor(const String & id, bool create);
    void wireWindow(DockableWindow & w, const String & id);
    void dockTool(const String & id);
    void toggleDock(const String & id);
    void showSlotMenu(int group, int slot);
    void showInstrumentMenu(int track);
    void showMidiDeviceMenu(int track);
    void loadLayout();
    void saveLayout();
    void rememberWindow(const String & id);
    void setParam(const String & id, float value);
    float getParam(const String & id) const;
    int findPeerIndexByName(const String & name) const;
    int findOpusFormat(int bitrate) const;

    SonobusAudioProcessor & processor;
    std::unique_ptr<VideoTilesView> page;
    LessonInfo info;
    Hooks hooks;

    std::unique_ptr<DockFrame> frame;             // the dock: a child window over the page's gap
    std::map<String, std::unique_ptr<DockableWindow>> ownWindows;  // Tools, Metronome, Tuner (closing one stops what it runs)
    std::unique_ptr<MixerWindow> mixerWindow;     // the mixer popped out (made when first shown)
    bool mixerOut = false;                        // the mixer opens in its own window
    String mixerWindowState;
    String docked;                                // the tool in the dock, or empty
    juce::Rectangle<float> toolsRect, sideRect;   // the page's gaps, as fractions; empty: not laid out yet
    std::map<String, bool> wantDocked;            // each tool's last place (docked unless popped out)
    std::map<String, String> windowStates;        // each tool's own window, where it was

    std::map<String, String> peerVideoIds;   // audio name -> video identity
    int lastPeerCount = -1;
    int ticks = 0;
    int chatIndex = 0;
    int64 joinedAtMs = 0;
    bool everConnected = false;
    float monitorLevel = 1.0f;   // "Hear myself" level to go back to
    float outputLevel = 1.0f;    // output level to go back to after the Output button (T-077)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LessonView)
};

}
