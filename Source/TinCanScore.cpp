// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanScore.h"
#include "TinCanPaths.h"
#include "TinCanScoreCore.h"
#include "TinCanScorePlayer.h"
#include "TinCanTheme.h"
#include "TinCanVideo.h"
#include "SonobusPluginProcessor.h"
#include "BinaryData.h"

namespace TinCan {

namespace {

// file:// address of the score page: written from the app's resources to %APPDATA%\CanDuet\score\
// each time (so it always matches this build); TINCAN_SCORE_PAGE=<folder> loads it from a folder.
String getScorePageURL()
{
    auto dev = SystemStats::getEnvironmentVariable("TINCAN_SCORE_PAGE", {}).trim();
    if (dev.isNotEmpty() && File::isAbsolutePath(dev) && File(dev).getChildFile("score.html").existsAsFile())
        return URL(File(dev).getChildFile("score.html")).toString(false);

    auto folder = TinCan::getDataFolder().getChildFile("score");
    folder.createDirectory();
    const struct { const char * name; const char * data; int size; } files[] = {
        { "score.html", BinaryData::score_html, BinaryData::score_htmlSize },
        { "score-model.js", BinaryData::scoremodel_js, BinaryData::scoremodel_jsSize },
        { "score-ui.js", BinaryData::scoreui_js, BinaryData::scoreui_jsSize },
        { "vexflow-bravura.js", BinaryData::vexflowbravura_js, BinaryData::vexflowbravura_jsSize },
    };
    for (auto & f : files) {
        auto file = folder.getChildFile(f.name);
        MemoryBlock existing;
        if (file.getSize() != f.size || !file.loadFileAsData(existing) || !existing.matches(f.data, (size_t) f.size))
            file.replaceWithData(f.data, (size_t) f.size);
    }
    return URL(folder.getChildFile("score.html")).toString(false);
}

}

#if JUCE_WEB_BROWSER && JUCE_USE_WIN_WEBVIEW2

// Stays on the score page; links and new windows never open. Messages go to the window.
class ScoreWindow::Browser : public WebBrowserComponent
{
public:
    Browser(const Options & options, const String & page_, ScoreWindow & owner_)
        : WebBrowserComponent(options), page(page_), owner(owner_) {}

    bool pageAboutToLoad(const String & newURL) override
    {
        return newURL.upToFirstOccurrenceOf("#", false, false) == page || newURL == "about:blank";
    }

    void newWindowAttemptingToLoad(const String &) override {}

    void webMessageReceived(const String & sourceURL, const String & message) override
    {
        if (sourceURL.upToFirstOccurrenceOf("#", false, false) != page) return;
        if (message.length() > 1024 * 1024) return;   // a play message with a long score is the biggest
        var m;
        if (JSON::parse(message, m).failed() || !m.isObject()) return;
        owner.handleMessage(m);
    }

private:
    String page;
    ScoreWindow & owner;
};

ScoreWindow::ScoreWindow(SonobusAudioProcessor & p, NoteShare & ns, Hooks h)
    : DockableWindow(TRANS("Score"), 1200, 720),
      processor(p), noteShare(ns), hooks(std::move(h))
{
    setResizeLimits(560, 320, 10000, 10000);

    auto options = makeWebViewOptions();
    if (WebBrowserComponent::areOptionsSupported(options)) {
        auto url = getScorePageURL();
        auto b = std::make_unique<Browser>(options, url, *this);
        String name = hooks.myName ? hooks.myName().substring(0, 80) : String();
        b->goToURL(url + "#name=" + URL::addEscapeChars(name, true) + (hooks.practice ? "&practice=1" : ""));
        browser = b.get();
        setBody(std::move(b));
    }
    else {
        auto label = std::make_unique<Label>(String(), TRANS("The score needs Microsoft Edge WebView2, which isn't installed on this computer."));
        label->setJustificationType(Justification::centred);
        setBody(std::move(label));
    }

    // the page draws its dock icons at the end of its own bar
    if (browser != nullptr)
        getHolder().setControlsInBody([this](bool docked, bool canDock) { postDock(docked, canDock); });

    noteShare.addListener(this);
    startTimerHz(30);
}

void ScoreWindow::postSound()
{
    DynamicObject::Ptr m = new DynamicObject();
    m->setProperty("t", "sound");
    m->setProperty("name", processor.getScoreInstrumentName().substring(0, 120));
    post(var(m.get()));
}

void ScoreWindow::postDock(bool docked, bool canDock)
{
    DynamicObject::Ptr m = new DynamicObject();
    m->setProperty("t", "dock");
    m->setProperty("docked", docked);
    m->setProperty("canDock", canDock);
    post(var(m.get()));
}

void ScoreWindow::showSoundMenu()
{
    auto & host = processor.getPluginHost();
    const auto instruments = host.getInstruments();
    const bool usingPlugin = processor.getScoreInstrumentName().isNotEmpty();
    const auto currentId = usingPlugin ? processor.getScorePlayer().getInstrument()->getPluginDescription().createIdentifierString() : String();

    PopupMenu menu;
    menu.addItem(1, TRANS("Built-in sounds"), true, !usingPlugin);
    menu.addSectionHeader(TRANS("VST3 instruments"));
    if (instruments.isEmpty()) menu.addItem(4, TRANS("None found yet: use Find plugins"), false, false);
    for (int i = 0; i < instruments.size(); ++i)
        menu.addItem(100 + i, instruments[i].name + (instruments[i].manufacturerName.isNotEmpty() ? "  (" + instruments[i].manufacturerName + ")" : String()),
                     true, instruments[i].createIdentifierString() == currentId);
    menu.addSeparator();
    if (usingPlugin) menu.addItem(2, TRANS("Show the instrument's window..."));
    menu.addItem(3, TRANS("Find plugins..."));

    Component::SafePointer<ScoreWindow> safeThis(this);
    menu.showMenuAsync(PopupMenu::Options().withMousePosition(), [safeThis, instruments](int result) {
        if (!safeThis || result == 0) return;
        auto & proc = safeThis->processor;
        if (result == 2) { proc.showScoreInstrumentWindow(); return; }
        if (result == 3) { proc.getPluginHost().showPluginList(); return; }
        String id;
        if (result >= 100 && result - 100 < instruments.size()) id = instruments[result - 100].createIdentifierString();
        else if (result != 1) return;
        String error;
        if (!proc.setScoreInstrument(id, error)) {
            AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, TRANS("Couldn't load the instrument"), error);
            return;
        }
        safeThis->postSound();
        if (id.isNotEmpty()) proc.showScoreInstrumentWindow();   // to pick its sound
    });
}

ScoreWindow::~ScoreWindow()
{
    noteShare.removeListener(this);
    stopTimer();
    if (processor.getScorePlayer().isPlaying()) stopPlaying();
}

void ScoreWindow::post(const var & message)
{
    if (browser && ready) browser->postWebMessage(JSON::toString(message, true));
}

void ScoreWindow::receive(const String & from, const String & payload)
{
    if (payload.getNumBytesAsUTF8() > (size_t) kScoreMaxPayload + 64) return;
    if (!ready) {
        if (early.size() < 400) early.emplace_back(from, payload);
        return;
    }
    DynamicObject::Ptr m = new DynamicObject();
    m->setProperty("t", "peer");
    m->setProperty("from", from.substring(0, 80));
    m->setProperty("data", payload);
    post(var(m.get()));
}

void ScoreWindow::handleMessage(const var & m)
{
    auto type = m.getProperty("t", var()).toString();
    if (type == "ready") {
        ready = true;
        DynamicObject::Ptr init = new DynamicObject();
        init->setProperty("t", "init");
        init->setProperty("name", hooks.myName ? hooks.myName().substring(0, 80) : String());
        init->setProperty("practice", hooks.practice);
        init->setProperty("bpm", getTempo());
        post(var(init.get()));
        lastTempo = getTempo();
        lastPeers.clear();
        ticks = 0;
        auto pending = std::move(early);
        early.clear();
        for (auto & e : pending) receive(e.first, e.second);
        postSound();
        postDock(getHolder().isDocked(), getHolder().canDockIn());
    }
    // the icons the page draws for the app (T-089)
    else if (type == "dockToggle") {
        getHolder().requestToggleDock();
    }
    else if (type == "dockClose") {
        // in its own window the page has no close icon (the title bar does), but just in case
        if (getHolder().isDocked()) getHolder().requestClose();
        else closeButtonPressed();
    }
    else if (type == "soundMenu") {
        showSoundMenu();
    }
    else if (type == "send") {
        auto data = m.getProperty("data", var()).toString();
        if (!hooks.practice && hooks.sendToPeers && data.isNotEmpty() && data.getNumBytesAsUTF8() <= (size_t) kScoreMaxPayload)
            hooks.sendToPeers(data);
    }
    else if (type == "play") {
        play(m);
    }
    else if (type == "record") {
        record(m);
    }
    else if (type == "stop") {
        stopPlaying();
    }
    else if (type == "tempo") {
        double bpm = m.getProperty("bpm", var(0.0));
        if (bpm >= 20.0 && bpm <= 300.0) {
            if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetTempo))
                p->setValueNotifyingHost(p->convertTo0to1((float) std::round(bpm)));
        }
    }
}

void ScoreWindow::play(const var & m)
{
    auto & player = processor.getScorePlayer();
    if (!player.load()) {
        DynamicObject::Ptr pos = new DynamicObject();
        pos->setProperty("t", "pos");
        pos->setProperty("beat", 0.0);
        pos->setProperty("playing", false);
        post(var(pos.get()));
        AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, TRANS("Score"), player.getError());
        return;
    }

    std::vector<ScoreNote> notes;
    if (auto * list = m.getProperty("events", var()).getArray()) {
        notes.reserve((size_t) jmin(list->size(), 20000));
        for (int i = 0; i < list->size() && i < 20000; ++i) {
            auto * e = list->getReference(i).getArray();
            if (!e || e->size() != 3) continue;
            ScoreNote n;
            n.beat = (*e)[0];
            n.len = (*e)[1];
            n.midi = (int) (*e)[2];
            notes.push_back(n);
        }
    }
    double end = m.getProperty("end", var(0.0));
    int program = jlimit(0, 127, (int) m.getProperty("program", var(0)));
    bool click = m.getProperty("click", var(true));
    int bpb = (int) m.getProperty("bpb", var(0));

    if (click) {
        // the bar length the score starts in (whole quarter notes only), then the metronome on
        if (bpb >= 1 && bpb <= 12) {
            if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetBeatsPerBar))
                p->setValueNotifyingHost(p->convertTo0to1((float) bpb));
        }
        if (!isMetronomeOn()) {
            setMetronomeOn(true);
            turnedMetronomeOn = true;
        }
    }
    player.play(std::move(notes), end, program, click);
    wasPlaying = true;
}

// Recording (T-080): the metronome starts again with a count-in of 1 or 2 bars, and the player runs
// on its clock playing nothing, so each MIDI note gets its score beat on the audio thread
// (beatAt). The page turns those into notes when recording stops.
void ScoreWindow::record(const var & m)
{
    auto & player = processor.getScorePlayer();
    if (!player.load()) {
        AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, TRANS("Score"), player.getError());
        return;
    }
    const int bars = jlimit(1, 2, (int) m.getProperty("countIn", var(1)));
    const int bpb = (int) m.getProperty("bpb", var(0));
    if (auto * v = processor.getValueTreeState().getRawParameterValue(SonobusAudioProcessor::paramMetCountIn))
        if (savedCountIn < 0) savedCountIn = roundToInt(v->load());
    setIntParam(SonobusAudioProcessor::paramMetCountIn, bars);
    if (bpb >= 1 && bpb <= 12) setIntParam(SonobusAudioProcessor::paramMetBeatsPerBar, bpb);

    // the count-in only plays when the metronome starts, so a running one is stopped first
    const bool wasOn = isMetronomeOn();
    if (wasOn) setMetronomeOn(false);
    else turnedMetronomeOn = true;
    Component::SafePointer<ScoreWindow> safe(this);
    Timer::callAfterDelay(wasOn ? 120 : 1, [safe]() {
        if (!safe) return;
        safe->setMetronomeOn(true);
        safe->processor.getScorePlayer().play({}, 1.0e7, 0, true);
    });
    wasPlaying = true;
}

void ScoreWindow::setIntParam(const String & id, int value)
{
    if (auto * p = processor.getValueTreeState().getParameter(id))
        p->setValueNotifyingHost(p->convertTo0to1((float) value));
}

void ScoreWindow::stopPlaying()
{
    processor.getScorePlayer().stop();
    if (turnedMetronomeOn) setMetronomeOn(false);
    turnedMetronomeOn = false;
    if (savedCountIn >= 0) setIntParam(SonobusAudioProcessor::paramMetCountIn, savedCountIn);
    savedCountIn = -1;
}

void ScoreWindow::setMetronomeOn(bool on)
{
    if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetEnabled))
        p->setValueNotifyingHost(on ? 1.0f : 0.0f);
}

bool ScoreWindow::isMetronomeOn() const
{
    if (auto * v = processor.getValueTreeState().getRawParameterValue(SonobusAudioProcessor::paramMetEnabled)) return v->load() > 0.5f;
    return false;
}

double ScoreWindow::getTempo() const
{
    if (auto * v = processor.getValueTreeState().getRawParameterValue(SonobusAudioProcessor::paramMetTempo)) return v->load();
    return 100.0;
}

void ScoreWindow::timerCallback()
{
    if (!ready) return;
    auto & player = processor.getScorePlayer();
    const bool playingNow = player.isPlaying();

    if (playingNow || wasPlaying) {
        double beat = player.getPosition();
        DynamicObject::Ptr pos = new DynamicObject();
        pos->setProperty("t", "pos");
        pos->setProperty("beat", beat < -1.0e8 ? -1.0 : beat);
        pos->setProperty("playing", playingNow);
        pos->setProperty("countIn", playingNow ? processor.getMetronomeCountInRemaining() : 0.0);
        post(var(pos.get()));
        if (!playingNow) {
            // finished by itself
            if (turnedMetronomeOn) setMetronomeOn(false);
            turnedMetronomeOn = false;
            if (savedCountIn >= 0) setIntParam(SonobusAudioProcessor::paramMetCountIn, savedCountIn);
            savedCountIn = -1;
            wasPlaying = false;
        }
    }

    if (++ticks % 6 == 0) {
        double t = getTempo();
        if (std::abs(t - lastTempo) > 0.001) {
            lastTempo = t;
            DynamicObject::Ptr msg = new DynamicObject();
            msg->setProperty("t", "tempo");
            msg->setProperty("bpm", t);
            post(var(msg.get()));
        }
    }
    if (ticks % 30 == 1) {
        StringArray names = (hooks.peerNames && !hooks.practice) ? hooks.peerNames() : StringArray();
        if (names != lastPeers) {
            lastPeers = names;
            Array<var> list;
            for (auto & n : names) list.add(n.substring(0, 80));
            DynamicObject::Ptr msg = new DynamicObject();
            msg->setProperty("t", "peers");
            msg->setProperty("names", list);
            post(var(msg.get()));
        }
    }
}

void ScoreWindow::notesArrived(const std::vector<NoteEvent> & events)
{
    // only this computer's MIDI keyboard writes notes into the score
    Array<var> list;
    for (const auto & ev : events) {
        if (ev.source != NoteSource::Midi) continue;
        Array<var> e;
        e.add((int) ev.type); e.add((int) ev.channel); e.add((int) ev.note); e.add((int) ev.value); e.add((int) ev.source);
        if (ev.beat > -1.0e8) e.add(ev.beat);   // score beat, while playing or recording
        list.add(var(e));
        if (list.size() >= 256) break;
    }
    if (list.isEmpty()) return;
    DynamicObject::Ptr msg = new DynamicObject();
    msg->setProperty("t", "notes");
    msg->setProperty("e", list);
    post(var(msg.get()));
}

#else

class ScoreWindow::Browser : public Component {};

ScoreWindow::ScoreWindow(SonobusAudioProcessor & p, NoteShare & ns, Hooks h)
    : DockableWindow(TRANS("Score"), 480, 240),
      processor(p), noteShare(ns), hooks(std::move(h))
{
    auto label = std::make_unique<Label>(String(), TRANS("The score isn't available in this build."));
    label->setJustificationType(Justification::centred);
    setBody(std::move(label));
}

ScoreWindow::~ScoreWindow() = default;
void ScoreWindow::receive(const String &, const String &) {}
void ScoreWindow::handleMessage(const var &) {}
void ScoreWindow::post(const var &) {}
void ScoreWindow::play(const var &) {}
void ScoreWindow::record(const var &) {}
void ScoreWindow::setIntParam(const String &, int) {}
void ScoreWindow::stopPlaying() {}
void ScoreWindow::setMetronomeOn(bool) {}
bool ScoreWindow::isMetronomeOn() const { return false; }
double ScoreWindow::getTempo() const { return 100.0; }
void ScoreWindow::timerCallback() {}
void ScoreWindow::notesArrived(const std::vector<NoteEvent> &) {}

#endif

//==============================================================================

TempoShare::TempoShare(SonobusAudioProcessor & p, std::function<void(const String &)> send,
                       std::function<bool()> lesson, std::function<String()> name)
    : processor(p), sendToPeers(std::move(send)), inLesson(std::move(lesson)), myName(std::move(name))
{
    lastSeen = getTempo();
    startTimer(200);
}

TempoShare::~TempoShare() { stopTimer(); }

double TempoShare::getTempo() const
{
    if (auto * v = processor.getValueTreeState().getRawParameterValue(SonobusAudioProcessor::paramMetTempo)) return v->load();
    return 100.0;
}

void TempoShare::timerCallback()
{
    const bool lesson = inLesson && inLesson();
    double t = getTempo();
    if (std::abs(t - lastSeen) > 0.001) {
        // changed here (the slider, the score page, tap tempo): a new counter, told to everyone
        lastSeen = t;
        if (lesson) {
            ++seq;
            seqBy = myName ? myName() : String();
            if (sendToPeers) sendToPeers(String(TempoWire::encode(t, seq)));
        }
        return;
    }
    // say it again every 5 s, so someone who missed it (or joined later) gets it
    if (lesson && seq > 0 && ++ticks % 25 == 0 && seqBy == (myName ? myName() : String())) {
        if (sendToPeers) sendToPeers(String(TempoWire::encode(t, seq)));
    }
}

void TempoShare::receive(const String & from, const String & payload)
{
    double bpm = 0;
    int64_t s = 0;
    if (!TempoWire::decode(payload.toStdString(), bpm, s)) return;
    if (!TempoWire::newer(s, from.toStdString(), seq, seqBy.toStdString())) return;
    seq = s;
    seqBy = from;
    lastSeen = std::round(bpm);   // not a change of ours, so it isn't sent back
    if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetTempo))
        p->setValueNotifyingHost(p->convertTo0to1((float) std::round(bpm)));
}

void TempoShare::lessonEnded()
{
    seq = 0;
    seqBy.clear();
    lastSeen = getTempo();
}

}
