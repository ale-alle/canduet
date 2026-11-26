// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanLesson.h"
#include "TinCanLessonRules.h"
#include "TinCanTheme.h"
#include "TinCanVideo.h"

#include <cmath>

namespace TinCan {

static bool isLocalhostHttp(const String & url)
{
    if (!url.startsWithIgnoreCase("http://") || url.containsChar('@')) return false;
    auto host = url.substring(7).upToFirstOccurrenceOf("/", false, false).upToFirstOccurrenceOf(":", false, false);
    return host == "localhost" || host == "127.0.0.1";
}

String getLessonPage()
{
    auto page = SystemStats::getEnvironmentVariable("TINCAN_LESSON_PAGE", {}).trim();
    if (page.startsWithIgnoreCase("https://") || isLocalhostHttp(page))
        return page.upToFirstOccurrenceOf("#", false, false);

    // next to the video page: …/app/video/ -> …/app/video/lesson.html
    auto video = getVideoPage();
    return video.upToLastOccurrenceOf("/", true, false) + "lesson.html";
}

String getPracticePage()
{
    auto page = SystemStats::getEnvironmentVariable("TINCAN_PRACTICE_PAGE", {}).trim();
    if (page.startsWithIgnoreCase("https://") || isLocalhostHttp(page))
        return page.upToFirstOccurrenceOf("#", false, false);

    auto file = File::getSpecialLocation(File::currentExecutableFile).getSiblingFile("lesson").getChildFile("lesson.html");
    return file.existsAsFile() ? URL(file).toString(false) : String();
}

#if JUCE_WEB_BROWSER && JUCE_USE_WIN_WEBVIEW2

// The dock (T-085): a borderless child window of the main window, laid exactly over the gap the page
// leaves for the docked tool. Created after the web view, so Windows draws it on top. It holds the
// docked tool's DockHolder, which moves in and out of the tool's own window.
class LessonView::DockFrame : public Component
{
public:
    DockFrame() { setOpaque(true); }
    void paint(Graphics & g) override { g.fillAll(TinCanTheme::surface); }
    void resized() override { for (auto * c : getChildren()) c->setBounds(getLocalBounds()); }
};

std::unique_ptr<LessonView> LessonView::create(SonobusAudioProcessor & processor, const VideoInfo & video,
                                               const LessonInfo & info, Hooks hooks)
{
    auto address = info.practice ? getPracticePage() : getLessonPage();
    if (address.isEmpty()) return nullptr;
    auto page = VideoTilesView::create(address, info.practice ? VideoInfo() : video, info.myName,
                                       info.practice ? "&practice=1" : "");
    if (!page) return nullptr;
    return std::unique_ptr<LessonView>(new LessonView(processor, std::move(page), info, std::move(hooks)));
}

LessonView::LessonView(SonobusAudioProcessor & p, std::unique_ptr<VideoTilesView> page_, const LessonInfo & info_, Hooks hooks_)
    : processor(p), page(std::move(page_)), info(info_), hooks(std::move(hooks_))
{
    setOpaque(true);
    joinedAtMs = Time::currentTimeMillis();
    {
        // only messages from now on; older chat belongs to an earlier lesson
        auto & events = processor.getAllChatEvents();
        const ScopedLock sl(events.getLock());
        chatIndex = events.size();
    }
    page->onMessage = [this](const var & message) { handleMessage(message); };
    // MIDI notes for lesson modules: Practice mode only for now, so lessons don't keep the tap on (QA)
    if (!info.practice) hooks.noteShare = nullptr;
    if (hooks.noteShare) hooks.noteShare->addListener(this);
    addAndMakeVisible(page.get());
    loadLayout();
    frame = std::make_unique<DockFrame>();
    startTimer(100);
}

LessonView::~LessonView()
{
    if (hooks.noteShare) hooks.noteShare->removeListener(this);
    stopTimer();
    // Board and Score outlive the lesson window: give a docked page back to its own window (hidden)
    // before the dock goes, or its WebView2 would go with it
    for (auto id : { "board", "score" }) {
        if (auto * w = windowFor(id, false)) {
            if (docked == id) w->takeBack();
            else if (w->isVisible()) rememberWindow(id);
            w->setVisible(false);
            w->onToggleDock = nullptr;
            w->onClose = nullptr;
            w->getHolder().setCanDock(false);
        }
    }
    if (toolsWindow && toolsWindow->isVisible()) rememberWindow("tools");
    saveLayout();
    toolsWindow.reset();
    frame.reset();
    page.reset();
}

void LessonView::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::night);
}

void LessonView::resized()
{
    page->setBounds(getLocalBounds());
    placeFrame();
}

ToolsPanel * LessonView::getTools() const
{
    return toolsWindow ? dynamic_cast<ToolsPanel *>(toolsWindow->getBody()) : nullptr;
}

// an Instrument strip's menu (T-086/T-087): no instrument, the found instruments, its window, find, remove the strip
void LessonView::showInstrumentMenu(int track)
{
    if (track < 0 || track >= processor.getNumLiveInstruments()) return;
    auto & host = processor.getPluginHost();
    const auto instruments = host.getInstruments();
    const auto current = processor.getLiveInstrumentName(track);

    PopupMenu menu;
    if (current.isNotEmpty()) {
        menu.addSectionHeader(current);
        menu.addItem(2, TRANS("Show its window..."));
        menu.addItem(1, TRANS("Remove the instrument"));
        menu.addSeparator();
    }
    menu.addSectionHeader(TRANS("VST3 instruments: play them with your MIDI keyboard"));
    if (instruments.isEmpty()) menu.addItem(5, TRANS("None found yet: use Find plugins"), false, false);
    for (int k = 0; k < instruments.size(); ++k)
        menu.addItem(100 + k, instruments[k].name + (instruments[k].manufacturerName.isNotEmpty() ? "  (" + instruments[k].manufacturerName + ")" : String()),
                     true, instruments[k].name == current);
    menu.addSeparator();
    menu.addItem(4, TRANS("Find plugins..."));
    if (processor.getNumLiveInstruments() > 1) {
        menu.addSeparator();
        menu.addItem(6, TRANS("Remove this strip"));
    }

    Component::SafePointer<LessonView> safeThis(this);
    menu.showMenuAsync(PopupMenu::Options().withMousePosition(), [safeThis, instruments, track](int result) {
        if (!safeThis || result == 0) return;
        auto & proc = safeThis->processor;
        if (track >= proc.getNumLiveInstruments()) return;
        if (result == 2) { proc.showLiveInstrumentWindow(track); return; }
        if (result == 4) { proc.getPluginHost().showPluginList(); return; }
        if (result == 6) { proc.removeLiveInstrumentTrack(track); safeThis->sendState(); return; }
        String id;
        if (result >= 100 && result - 100 < instruments.size()) id = instruments[result - 100].createIdentifierString();
        else if (result != 1) return;
        String error;
        if (!proc.setLiveInstrument(track, id, error)) {
            AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, TRANS("Couldn't load the instrument"), error);
            return;
        }
        safeThis->sendState();
        if (id.isNotEmpty()) proc.showLiveInstrumentWindow(track);   // to pick its sound
    });
}

// which MIDI keyboard plays an Instrument strip (T-087): all of them, or one
void LessonView::showMidiDeviceMenu(int track)
{
    if (track < 0 || track >= processor.getNumLiveInstruments()) return;
    const auto devices = MidiInput::getAvailableDevices();
    const auto current = processor.getLiveInstrumentDevice(track);

    PopupMenu menu;
    menu.addSectionHeader(TRANS("Played by"));
    menu.addItem(1, TRANS("All MIDI keyboards"), true, current.isEmpty());
    bool found = current.isEmpty();
    for (int k = 0; k < devices.size(); ++k) {
        const bool on = devices[k].identifier == current;
        found = found || on;
        menu.addItem(100 + k, devices[k].name, true, on);
    }
    if (!found) menu.addItem(2, processor.getLiveInstrumentDeviceName(track) + "  " + TRANS("(not connected)"), false, true);
    if (devices.isEmpty()) menu.addItem(3, TRANS("No MIDI keyboard found: plug one in"), false, false);

    Component::SafePointer<LessonView> safeThis(this);
    menu.showMenuAsync(PopupMenu::Options().withMousePosition(), [safeThis, devices, track](int result) {
        if (!safeThis || result == 0) return;
        auto & proc = safeThis->processor;
        if (track >= proc.getNumLiveInstruments()) return;
        if (result == 1) proc.setLiveInstrumentDevice(track, String());
        else if (result >= 100 && result - 100 < devices.size()) proc.setLiveInstrumentDevice(track, devices[result - 100].identifier);
        else return;
        safeThis->sendState();
    });
}

// a strip's VST3 effect slot (T-086): no effect, the found effects by maker, its window, bypass, find
void LessonView::showSlotMenu(int g, int s)
{
    auto & host = processor.getPluginHost();
    const auto effects = host.getEffects();
    const auto current = processor.getInputInsertName(g, s);
    const bool bypassed = processor.getInputInsertBypass(g, s);
    String currentId;
    if (current.isNotEmpty())
        for (auto & d : effects) if (d.name == current) { currentId = d.createIdentifierString(); break; }

    PopupMenu menu;
    if (current.isNotEmpty()) {
        menu.addSectionHeader(current);
        menu.addItem(2, TRANS("Show its window..."));
        menu.addItem(3, TRANS("Bypass"), true, bypassed);
        menu.addItem(1, TRANS("Remove"));
        menu.addSeparator();
    }
    menu.addSectionHeader(TRANS("VST3 effects"));
    if (effects.isEmpty()) menu.addItem(5, TRANS("None found yet: use Find plugins"), false, false);
    // grouped by maker once there are many
    std::map<String, PopupMenu> byMaker;
    for (int k = 0; k < effects.size(); ++k) {
        const auto & d = effects[k];
        const bool ticked = d.createIdentifierString() == currentId;
        if (effects.size() > 16) byMaker[d.manufacturerName.isNotEmpty() ? d.manufacturerName : TRANS("Other")].addItem(100 + k, d.name, true, ticked);
        else menu.addItem(100 + k, d.name + (d.manufacturerName.isNotEmpty() ? "  (" + d.manufacturerName + ")" : String()), true, ticked);
    }
    for (auto & [maker, sub] : byMaker) menu.addSubMenu(maker, sub);
    menu.addSeparator();
    menu.addItem(4, TRANS("Find plugins..."));

    Component::SafePointer<LessonView> safeThis(this);
    menu.showMenuAsync(PopupMenu::Options().withMousePosition(), [safeThis, effects, g, s](int result) {
        if (!safeThis || result == 0) return;
        auto & proc = safeThis->processor;
        if (result == 2) { proc.showInputInsertWindow(g, s); return; }
        if (result == 3) { proc.setInputInsertBypass(g, s, !proc.getInputInsertBypass(g, s)); safeThis->sendState(); return; }
        if (result == 4) { proc.getPluginHost().showPluginList(); return; }
        String id;
        if (result >= 100 && result - 100 < effects.size()) id = effects[result - 100].createIdentifierString();
        else if (result != 1) return;
        String error;
        if (!proc.setInputInsert(g, s, id, error)) {
            AlertWindow::showMessageBoxAsync(MessageBoxIconType::WarningIcon, TRANS("Couldn't load the effect"), error);
            return;
        }
        safeThis->sendState();
        if (id.isNotEmpty()) proc.showInputInsertWindow(g, s);   // to set it up
    });
}

// ---- docking (T-085) ----

void LessonView::loadLayout()
{
    const var v = JSON::parse(processor.getDockLayout());
    for (auto id : { "tools", "board", "score" }) {
        const var t = v.getProperty(id, var());
        if (!t.isObject()) continue;
        const var d = t.getProperty("docked", var());
        if (d.isBool()) wantDocked[id] = (bool) d;
        const auto state = t.getProperty("window", var()).toString();
        if (state.length() < 200) windowStates[id] = state;
    }
}

void LessonView::saveLayout()
{
    DynamicObject::Ptr all = new DynamicObject();
    for (auto id : { "tools", "board", "score" }) {
        DynamicObject::Ptr t = new DynamicObject();
        t->setProperty("docked", wantDocked.count(id) ? wantDocked[id] : true);
        if (windowStates.count(id)) t->setProperty("window", windowStates[id]);
        all->setProperty(id, var(t.get()));
    }
    processor.setDockLayout(JSON::toString(var(all.get()), true));
}

void LessonView::rememberWindow(const String & id)
{
    if (auto * w = windowFor(id, false); w != nullptr && !w->isDocked() && !w->isMinimised())
        windowStates[id] = w->saveState();
}

DockableWindow * LessonView::windowFor(const String & id, bool create)
{
    if (id == "tools") {
        if (!toolsWindow && create && hooks.makeTools) {
            if (auto panel = hooks.makeTools()) {
                toolsWindow = std::make_unique<DockableWindow>(TRANS("Tools"), 1100, 400);
                toolsWindow->setBody(std::move(panel));
            }
        }
        if (toolsWindow && create) wireWindow(*toolsWindow, id);
        return toolsWindow.get();
    }
    auto & hook = id == "board" ? hooks.boardWindow : hooks.scoreWindow;
    if (!hook || (id != "board" && id != "score")) return nullptr;
    auto * w = hook(create);
    if (w != nullptr && create) wireWindow(*w, id);
    return w;
}

void LessonView::wireWindow(DockableWindow & w, const String & id)
{
    // after the click returns: closing Tools deletes the window the button is in
    Component::SafePointer<LessonView> safeThis(this);
    w.onToggleDock = [safeThis, id]() { MessageManager::callAsync([safeThis, id]() { if (safeThis) safeThis->toggleDock(id); }); };
    w.onClose = [safeThis, id]() { MessageManager::callAsync([safeThis, id]() { if (safeThis) safeThis->closeTool(id); }); };
    w.getHolder().setCanDock(true);
    if (!w.getProperties().contains("canduetPlaced") && !w.isVisible()) {
        w.getProperties().set("canduetPlaced", true);
        if (windowStates.count(id)) w.restoreState(windowStates[id]);
    }
}

bool LessonView::isToolOpen(const String & id)
{
    if (docked == id) return true;
    auto * w = windowFor(id, false);
    return w != nullptr && w->isVisible();
}

void LessonView::openTool(const String & id)
{
    auto * w = windowFor(id, true);
    if (w == nullptr) return;
    const bool dockIt = wantDocked.count(id) ? wantDocked[id] : true;
    if (dockIt) dockTool(id);
    else if (docked != id) w->showFloating();
    sendState();
}

void LessonView::closeTool(const String & id)
{
    if (auto * w = windowFor(id, false)) {
        if (docked == id) {
            w->takeBack();
            docked.clear();
        }
        else if (w->isVisible()) rememberWindow(id);
        w->setVisible(false);
    }
    if (id == "tools") toolsWindow.reset();   // stops the tuner and the MIDI copy
    placeFrame();
    saveLayout();
    sendState();
}

void LessonView::dockTool(const String & id)
{
    // one docked tool at a time; tools in their own windows stay
    if (docked.isNotEmpty() && docked != id) closeTool(docked);
    auto * w = windowFor(id, true);
    if (w == nullptr) return;
    if (w->isVisible()) rememberWindow(id);
    // on the main window before the page moves in, so its WebView2 has a window to move to
    if (auto * top = getTopLevelComponent(); top != nullptr && top->getPeer() != nullptr && !frame->isOnDesktop())
        frame->addToDesktop(0, top->getPeer()->getNativeHandle());
    w->dockInto(*frame);
    docked = id;
    sendState();   // the page opens the gap, then tells us where it is
    placeFrame();
}

void LessonView::toggleDock(const String & id)
{
    auto * w = windowFor(id, false);
    if (w == nullptr) return;
    if (docked == id) {
        // pop out: its own window, where it was; the people get the whole window again
        wantDocked[id] = false;
        w->takeBack();
        docked.clear();
        if (windowStates.count(id)) w->restoreState(windowStates[id]);
        w->showFloating();
        placeFrame();
    }
    else {
        wantDocked[id] = true;
        dockTool(id);
    }
    saveLayout();
    sendState();
}

void LessonView::placeFrame()
{
    if (!frame) return;
    auto * top = getTopLevelComponent();
    auto * peer = top ? top->getPeer() : nullptr;
    const auto rect = docked == "tools" ? toolsRect : sideRect;
    if (docked.isEmpty() || peer == nullptr || rect.isEmpty()) {
        frame->setVisible(false);
        return;
    }
    if (!frame->isOnDesktop()) {
        // a child of the main window's native window, created after the web view so it sits above it
        frame->addToDesktop(0, peer->getNativeHandle());
    }

    auto area = page->getLocalBounds().toFloat();
    juce::Rectangle<int> r = juce::Rectangle<float>(area.getWidth() * rect.getX(), area.getHeight() * rect.getY(),
                                                    area.getWidth() * rect.getWidth(), area.getHeight() * rect.getHeight())
                                 .getSmallestIntegerContainer();
    // child windows are placed in the main window's client coordinates
    auto bounds = top->getLocalArea(page.get(), r);
    if (frame->getBounds() != bounds) frame->setBounds(bounds);
    if (!frame->isVisible()) {
        frame->setVisible(true);
        frame->toFront(false);   // above the web view's own window
    }
}

void LessonView::setPeerVideoId(const String & userName, const String & identity)
{
    if (userName.isEmpty() || !isValidVideoIdentity(identity) || peerVideoIds.size() > 64) return;
    peerVideoIds[userName] = identity;
}

void LessonView::showToast(const String & text, int timeoutMs)
{
    DynamicObject::Ptr m = new DynamicObject();
    m->setProperty("t", "toast");
    m->setProperty("text", text.substring(0, 400));
    m->setProperty("ms", timeoutMs);
    page->send(var(m.get()));
}

void LessonView::timerCallback()
{
    // tell everyone our video identity when someone joins, and every 10 s for anyone who missed it
    int count = processor.getNumberRemotePeers();
    if (count != lastPeerCount || ++ticks >= 100) {
        lastPeerCount = count;
        ticks = 0;
        if (count > 0 && hooks.shareVideoId) hooks.shareVideoId();
        // forget video identities of people who left, so the table never fills up in a long lesson
        for (auto it = peerVideoIds.begin(); it != peerVideoIds.end(); ) {
            if (findPeerIndexByName(it->first) < 0) it = peerVideoIds.erase(it);
            else ++it;
        }
    }
    sendState();
    sendNewChat();
    placeFrame();   // the main window may have moved or changed size
}

float LessonView::getParam(const String & id) const
{
    if (auto * prm = processor.getValueTreeState().getParameter(id))
        return prm->convertFrom0to1(prm->getValue());
    return 0.0f;
}

void LessonView::setParam(const String & id, float value)
{
    if (auto * prm = processor.getValueTreeState().getParameter(id))
        prm->setValueNotifyingHost(prm->convertTo0to1(value));
}

int LessonView::findPeerIndexByName(const String & name) const
{
    if (name.isEmpty()) return -1;
    for (int i = 0; i < processor.getNumberRemotePeers(); ++i) {
        if (processor.getRemotePeerUserName(i) == name) return i;
    }
    return -1;
}

int LessonView::findOpusFormat(int bitrate) const
{
    SonobusAudioProcessor::AudioCodecFormatInfo f;
    for (int i = 0; i < processor.getNumberAudioCodecFormats(); ++i) {
        if (processor.getAudioCodeFormatInfo(i, f) && f.codec == SonobusAudioProcessor::CodecOpus && f.bitrate == bitrate) return i;
    }
    return -1;
}

static float meterLevel(foleys::LevelMeterSource * meter)
{
    if (meter == nullptr) return 0.0f;
    float rms = 0.0f;
    for (int ch = 0; ch < meter->getNumChannels(); ++ch) rms = jmax(rms, meter->getRMSLevel(ch));
    float level = jlimit(0.0f, 1.0f, (Decibels::gainToDecibels(rms, -60.0f) + 60.0f) / 60.0f);
    return std::round(level * 100.0f) / 100.0f;
}

static float round1(float v) { return std::round(v * 10.0f) / 10.0f; }

// the input mixer's rows (T-077): one per input group, in order. Its meter channels are the groups'
// channels laid end to end, after each input's level (and the microphone level over them all).
// one input's effects (T-084): each knob's amount (0 = off) and every setting behind its pop-up
static var compressorVar(const SonoAudio::CompressorParams & p, bool withMakeup)
{
    DynamicObject::Ptr o = new DynamicObject();
    o->setProperty("amount", p.enabled ? p.mix : 0.0f);
    o->setProperty("threshold", p.thresholdDb);
    o->setProperty("ratio", p.ratio);
    o->setProperty("attack", p.attackMs);
    o->setProperty("release", p.releaseMs);
    if (withMakeup) {
        o->setProperty("makeup", p.makeupGainDb);
        o->setProperty("autoMakeup", p.automakeupGain);
    }
    return var(o.get());
}

var LessonView::effectsState(int g)
{
    DynamicObject::Ptr fx = new DynamicObject();
    SonoAudio::CompressorParams gate, comp;
    SonoAudio::ParametricEqParams eq;
    processor.getInputExpanderParams(g, gate);
    processor.getInputCompressorParams(g, comp);
    processor.getInputEqParams(g, eq);
    fx->setProperty("gate", compressorVar(gate, false));
    fx->setProperty("comp", compressorVar(comp, true));
    DynamicObject::Ptr e = new DynamicObject();
    e->setProperty("amount", eq.enabled ? eq.mix : 0.0f);
    e->setProperty("lowGain", eq.lowShelfGain);   e->setProperty("lowFreq", eq.lowShelfFreq);
    e->setProperty("mid1Gain", eq.para1Gain);     e->setProperty("mid1Freq", eq.para1Freq);   e->setProperty("mid1Q", eq.para1Q);
    e->setProperty("mid2Gain", eq.para2Gain);     e->setProperty("mid2Freq", eq.para2Freq);   e->setProperty("mid2Q", eq.para2Q);
    e->setProperty("highGain", eq.highShelfGain); e->setProperty("highFreq", eq.highShelfFreq);
    fx->setProperty("eq", var(e.get()));
    fx->setProperty("rev", processor.getInputReverbSend(g, true));
    fx->setProperty("dly", processor.getInputDelaySend(g));
    // VST3 effect slots (T-086): the plugin's name and whether it's bypassed, or null when empty
    Array<var> slots;
    for (int s = 0; s < InsertChain::numSlots; ++s) {
        const auto name = processor.getInputInsertName(g, s);
        if (name.isEmpty()) { slots.add(var()); continue; }
        DynamicObject::Ptr o = new DynamicObject();
        o->setProperty("name", name.substring(0, 60));
        o->setProperty("bypass", processor.getInputInsertBypass(g, s));
        slots.add(var(o.get()));
    }
    fx->setProperty("slots", slots);
    return var(fx.get());
}

// the shared Reverb and Delay the strips send to; everyone hears them (T-084)
var LessonView::busState()
{
    DynamicObject::Ptr bus = new DynamicObject();
    DynamicObject::Ptr rev = new DynamicObject();
    rev->setProperty("level", getParam(SonobusAudioProcessor::paramInputReverbLevel));
    rev->setProperty("size", getParam(SonobusAudioProcessor::paramInputReverbSize));
    rev->setProperty("damping", getParam(SonobusAudioProcessor::paramInputReverbDamping));
    rev->setProperty("preDelay", getParam(SonobusAudioProcessor::paramInputReverbPreDelay));
    bus->setProperty("rev", var(rev.get()));
    auto & d = processor.getInputDelay();
    DynamicObject::Ptr dly = new DynamicObject();
    dly->setProperty("time", d.timeMs.load());
    dly->setProperty("repeats", d.feedback.load());
    dly->setProperty("tone", d.toneHz.load());
    dly->setProperty("level", d.level.load());
    bus->setProperty("dly", var(dly.get()));
    return var(bus.get());
}

var LessonView::inputsState()
{
    Array<int> numbers;
    StringArray names;
    if (hooks.inputChannels) hooks.inputChannels(numbers, names);

    auto & meter = processor.getPostInputMeterSource();
    Array<var> rows;
    int meterCh = 0;
    for (int g = 0; g < processor.getInputGroupCount(); ++g) {
        int start = 0, count = 1;
        processor.getInputGroupChannelStartAndCount(g, start, count);
        auto number = [&](int ch) { return (ch >= 0 && ch < numbers.size() ? numbers[ch] : ch) + 1; };
        auto name = [&](int ch) { return ch >= 0 && ch < names.size() ? names[ch].substring(0, 60) : String(); };
        const bool stereo = count == 2;

        DynamicObject::Ptr r = new DynamicObject();
        r->setProperty("i", g);
        r->setProperty("label", stereo ? "Input " + String(number(start)) + "–" + String(number(start + 1))
                                       : "Input " + String(number(start)));
        String nm = name(start);
        if (stereo && name(start + 1).isNotEmpty() && name(start + 1) != nm) nm << " / " << name(start + 1);
        r->setProperty("name", nm);
        r->setProperty("stereo", stereo);
        // a single input can pair with the next one when that is a single input too
        int ns = 0, nc = 0;
        r->setProperty("canPair", !stereo && g + 1 < processor.getInputGroupCount()
                                  && processor.getInputGroupChannelStartAndCount(g + 1, ns, nc) && nc == 1 && ns == start + 1);
        r->setProperty("gain", processor.getInputGroupGain(g));
        // a pair's position, as set by "pan" below: the side that isn't at the edge, back from it
        float pan = processor.getInputChannelPan(g, 0);
        if (stereo) {
            float right = processor.getInputChannelPan(g, 1);
            pan = right < 0.999f ? right - 1.0f : pan + 1.0f;
        }
        r->setProperty("pan", round1(pan * 10.0f) / 10.0f);
        r->setProperty("muted", processor.getInputGroupMuted(g));
        r->setProperty("fx", effectsState(g));
        // one meter per channel (a pair's strip shows two), 0..1 over -60..0 dB
        Array<var> levels;
        float top = 0.0f;
        for (int c = meterCh; c < meterCh + count; ++c) {
            float rms = c < meter.getNumChannels() ? meter.getRMSLevel(c) : 0.0f;
            float level = jlimit(0.0f, 1.0f, (Decibels::gainToDecibels(rms, -60.0f) + 60.0f) / 60.0f);
            level = std::round(level * 100.0f) / 100.0f;
            levels.add(level);
            top = jmax(top, level);
        }
        r->setProperty("level", top);
        r->setProperty("levels", levels);
        meterCh += count;
        rows.add(var(r.get()));
    }
    return rows;
}

void LessonView::sendState()
{
    using namespace LessonRules;

    Array<var> peers;
    for (int i = 0; i < processor.getNumberRemotePeers(); ++i) {
        auto name = processor.getRemotePeerUserName(i);
        if (name.isEmpty()) continue;

        int start = 0, count = 0;
        bool stereo = processor.getRemotePeerChannelGroupStartAndCount(i, 0, start, count) && count == 2;
        float pan = processor.getRemotePeerChannelPan(i, 0, 0);
        if (stereo) pan = 0.5f * (pan + processor.getRemotePeerChannelPan(i, 0, 1));

        DynamicObject::Ptr p = new DynamicObject();
        p->setProperty("id", name);
        auto vid = peerVideoIds.find(name);
        if (vid != peerVideoIds.end()) p->setProperty("vid", vid->second);
        p->setProperty("gain", processor.getRemotePeerLevelGain(i));
        p->setProperty("pan", pan);
        p->setProperty("stereo", stereo);
        p->setProperty("muted", !processor.getRemotePeerRecvAllow(i));
        p->setProperty("soloed", processor.getRemotePeerSoloed(i));
        p->setProperty("connected", processor.getRemotePeerConnected(i));
        p->setProperty("level", meterLevel(processor.getRemotePeerRecvMeterSource(i)));

        SonobusAudioProcessor::LatencyInfo lat;
        if (processor.getRemotePeerLatencyInfo(i, lat)) {
            p->setProperty("delayMs", round1(oneWayDelayMs(lat.outgoingMs, lat.totalRoundtripMs)));
            p->setProperty("pingMs", round1(lat.pingMs));
        }

        bool initDone = false;
        p->setProperty("bufferMs", round1(processor.getRemotePeerBufferTime(i)));
        p->setProperty("bufMode", String(bufferChoiceName(bufferChoiceFor((int) processor.getRemotePeerAutoresizeBufferMode(i, initDone)))));

        SonobusAudioProcessor::AudioCodecFormatInfo f;
        int requested = processor.getRequestRemotePeerSendAudioCodecFormat(i);
        bool haveRequested = requested >= 0 && processor.getAudioCodeFormatInfo(requested, f);
        p->setProperty("quality", String(qualityName(qualityFor(haveRequested ? requested : -1,
                                                                haveRequested && f.codec == SonobusAudioProcessor::CodecOpus,
                                                                haveRequested ? f.bitrate : 0))));
        if (processor.getRemotePeerReceiveAudioCodecFormat(i, f))
            p->setProperty("recvFormat", f.name + (processor.getRemotePeerRecvChannelCount(i) == 2 ? ", stereo" : ", mono"));
        if (processor.getAudioCodeFormatInfo(processor.getRemotePeerAudioCodecFormat(i), f))
            p->setProperty("sendFormat", f.name);

        auto received = processor.getRemotePeerPacketsReceived(i);
        if (received > 0) p->setProperty("dropped", round1(100.0f * (float) processor.getRemotePeerPacketsDropped(i) / (float) received));

        peers.add(var(p.get()));
    }

    DynamicObject::Ptr me = new DynamicObject();
    me->setProperty("name", info.myName);
    me->setProperty("role", info.teacher ? "teacher" : "learner");
    me->setProperty("micOff", getParam(SonobusAudioProcessor::paramMainSendMute) > 0.5f);
    me->setProperty("inGain", getParam(SonobusAudioProcessor::paramInGain));
    me->setProperty("monitor", getParam(SonobusAudioProcessor::paramDry) > 0.001f);
    me->setProperty("monitorLevel", getParam(SonobusAudioProcessor::paramDry));   // 0..1; pages with the slider use this
    me->setProperty("recvMute", getParam(SonobusAudioProcessor::paramMainRecvMute) > 0.5f);
    me->setProperty("volume", getParam(SonobusAudioProcessor::paramWet));
    me->setProperty("outMute", getParam(SonobusAudioProcessor::paramWet) < 0.001f);   // the Output button (T-077)
    me->setProperty("outLevel", meterLevel(&processor.getOutputMeterSource()));
    me->setProperty("level", meterLevel(&processor.getPostInputMeterSource()));   // after the microphone level (T-077)
    if (hooks.deviceNames) {
        String in, out;
        hooks.deviceNames(in, out);
        me->setProperty("inDevice", in.substring(0, 80));
        me->setProperty("outDevice", out.substring(0, 80));
    }

    // the lesson room: connecting at first, connected once joined, lost if it goes away
    bool connected = processor.isConnectedToServer() && processor.getCurrentJoinedGroup().isNotEmpty();
    if (connected) everConnected = true;
    String conn = info.practice ? "practice"
                : connected ? "connected"
                : (!everConnected && Time::currentTimeMillis() - joinedAtMs < 20000) ? "connecting" : "lost";

    DynamicObject::Ptr lesson = new DynamicObject();
    lesson->setProperty("title", info.title);
    lesson->setProperty("startsAt", info.startsAtMs);
    lesson->setProperty("endsAt", info.endsAtMs);
    lesson->setProperty("joinedAt", joinedAtMs);

    DynamicObject::Ptr rec = new DynamicObject();
    rec->setProperty("phase", hooks.recordPhase ? hooks.recordPhase() : String("idle"));
    rec->setProperty("secs", hooks.recordSeconds ? std::floor(hooks.recordSeconds()) : 0.0);

    DynamicObject::Ptr docks = new DynamicObject();
    docks->setProperty("tools", isToolOpen("tools"));
    docks->setProperty("board", isToolOpen("board"));
    docks->setProperty("score", isToolOpen("score"));
    docks->setProperty("docked", docked);

    DynamicObject::Ptr msg = new DynamicObject();
    msg->setProperty("t", "state");
    msg->setProperty("me", var(me.get()));
    msg->setProperty("inputs", inputsState());
    msg->setProperty("fxBus", busState());
    {
        // the Instrument strips (T-086/T-087) and which keyboard plays each
        Array<var> list;
        // the device list is looked up once a second, not on every state message
        static Array<MidiDeviceInfo> devices;
        static uint32 devicesAt = 0;
        const uint32 nowMs = Time::getMillisecondCounter();
        if (devicesAt == 0 || nowMs - devicesAt > 1000) { devices = MidiInput::getAvailableDevices(); devicesAt = nowMs; }
        for (int k = 0; k < processor.getNumLiveInstruments(); ++k) {
            auto & li = processor.getLiveInstrument(k);
            const auto devId = processor.getLiveInstrumentDevice(k);
            bool connected = devId.isEmpty();
            for (auto & d : devices) connected = connected || d.identifier == devId;
            DynamicObject::Ptr inst = new DynamicObject();
            inst->setProperty("name", processor.getLiveInstrumentName(k).substring(0, 60));
            inst->setProperty("gain", li.gain.load());
            inst->setProperty("muted", li.muted.load());
            inst->setProperty("level", std::round(li.meter.load() * 100.0f) / 100.0f);
            inst->setProperty("device", devId.isEmpty() ? String() : processor.getLiveInstrumentDeviceName(k).substring(0, 60));
            inst->setProperty("connected", connected);
            list.add(var(inst.get()));
        }
        msg->setProperty("instruments", list);
        msg->setProperty("maxInstruments", (int) SonobusAudioProcessor::maxLiveInstruments);
        // T-087: how busy the audio is, and dropouts since the sound device started
        DynamicObject::Ptr cpu = new DynamicObject();
        cpu->setProperty("load", std::round(processor.getAudioLoad() * 100.0f) / 100.0f);
        cpu->setProperty("dropouts", processor.getAudioDropouts());
        msg->setProperty("cpu", var(cpu.get()));
    }
    msg->setProperty("conn", conn);
    msg->setProperty("lesson", var(lesson.get()));
    msg->setProperty("peers", peers);
    msg->setProperty("rec", var(rec.get()));
    msg->setProperty("docks", var(docks.get()));
    page->send(var(msg.get()));
}

void LessonView::notesArrived(const std::vector<NoteEvent> & events)
{
    // for lesson modules in the page (T-075): [type, channel, note, value, source], 256 per message,
    // all of them (a dropped note-off would leave a module's key held)
    for (size_t i = 0; i < events.size(); i += 256) {
        Array<var> list;
        for (size_t k = i; k < events.size() && k < i + 256; ++k) {
            const auto & ev = events[k];
            Array<var> e;
            e.add((int) ev.type); e.add((int) ev.channel); e.add((int) ev.note); e.add((int) ev.value); e.add((int) ev.source);
            list.add(var(e));
        }
        DynamicObject::Ptr msg = new DynamicObject();
        msg->setProperty("t", "notes");
        msg->setProperty("e", list);
        page->send(var(msg.get()));
    }
}

void LessonView::sendNewChat()
{
    Array<var> items;
    {
        auto & events = processor.getAllChatEvents();
        const ScopedLock sl(events.getLock());
        if (chatIndex > events.size()) chatIndex = events.size();   // the list was cleared
        for (; chatIndex < events.size() && items.size() < 200; ++chatIndex) {
            const auto & ev = events.getReference(chatIndex);
            // people's messages only: no video ids or recording messages (tags), and none of SonoBus's
            // own "joined group" lines (the tiles already show who is here)
            if (ev.tags.isNotEmpty() || ev.message.isEmpty() || ev.type == SBChatEvent::SystemType) continue;
            DynamicObject::Ptr m = new DynamicObject();
            m->setProperty("from", ev.from.substring(0, 80));
            m->setProperty("text", ev.message.substring(0, 2000));
            if (ev.type == SBChatEvent::SelfType) m->setProperty("me", true);
            items.add(var(m.get()));
        }
    }
    if (items.isEmpty()) return;
    DynamicObject::Ptr msg = new DynamicObject();
    msg->setProperty("t", "chat");
    msg->setProperty("items", items);
    page->send(var(msg.get()));
}

void LessonView::handleMessage(const var & message)
{
    const auto type = message.getProperty("t", var()).toString();
    const auto value = message.getProperty("v", var());
    const bool isNumber = value.isDouble() || value.isInt();

    if (type == "ready") {
        sendState();
    }
    else if (type == "gain" || type == "pan") {
        int i = findPeerIndexByName(message.getProperty("id", var()).toString());
        if (i < 0 || !isNumber) return;
        if (type == "gain") {
            processor.setRemotePeerLevelGain(i, jlimit(0.0f, 2.0f, (float) value));
            return;
        }
        float centre = jlimit(-1.0f, 1.0f, (float) value);
        int start = 0, count = 0;
        if (processor.getRemotePeerChannelGroupStartAndCount(i, 0, start, count) && count == 2) {
            // stereo: move both sides together and keep their spread
            float p1 = processor.getRemotePeerChannelPan(i, 0, 0), p2 = processor.getRemotePeerChannelPan(i, 0, 1);
            float half = 0.5f * (p2 - p1);
            processor.setRemotePeerChannelPan(i, 0, 0, jlimit(-1.0f, 1.0f, centre - half));
            processor.setRemotePeerChannelPan(i, 0, 1, jlimit(-1.0f, 1.0f, centre + half));
        }
        else {
            processor.setRemotePeerChannelPan(i, 0, 0, centre);
        }
    }
    else if (type == "peer") {
        int i = findPeerIndexByName(message.getProperty("id", var()).toString());
        if (i >= 0) handlePeerMessage(i, message.getProperty("k", var()).toString(), value);
    }
    else if (type == "mic") {
        setParam(SonobusAudioProcessor::paramMainSendMute, (bool) message.getProperty("off", var()) ? 1.0f : 0.0f);
    }
    else if (type == "inGain" && isNumber) {
        setParam(SonobusAudioProcessor::paramInGain, jlimit(0.0f, 4.0f, (float) value));
    }
    else if (type == "volume" && isNumber) {
        setParam(SonobusAudioProcessor::paramWet, jlimit(0.0f, 2.0f, (float) value));
    }
    else if (type == "monitor") {
        // keep a monitor level set in All settings: off remembers it, on brings it back
        float now = getParam(SonobusAudioProcessor::paramDry);
        if ((bool) value) setParam(SonobusAudioProcessor::paramDry, now > 0.001f ? now : monitorLevel);
        else {
            if (now > 0.001f) monitorLevel = now;
            setParam(SonobusAudioProcessor::paramDry, 0.0f);
        }
    }
    else if (type == "input") {
        // the input mixer (T-077): one input's level (0..4), pan or a pair's position (-1..1), mute, pair
        const var iv = message.getProperty("i", var());
        if (!(iv.isInt() || iv.isDouble())) return;
        const int g = (int) iv;
        if (g < 0 || g >= processor.getInputGroupCount()) return;
        const auto k = message.getProperty("k", var()).toString();
        int start = 0, count = 1;
        processor.getInputGroupChannelStartAndCount(g, start, count);
        if (k == "gain" && isNumber) {
            processor.setInputGroupGain(g, jlimit(0.0f, 4.0f, (float) value));
        }
        else if (k == "pan" && isNumber) {
            float p = jlimit(-1.0f, 1.0f, (float) value);
            if (count == 2) {
                // keep the pair full width and move it: right of centre narrows the left side, and back
                processor.setInputChannelPan(g, 0, jlimit(-1.0f, 1.0f, p - 1.0f));
                processor.setInputChannelPan(g, 1, jlimit(-1.0f, 1.0f, p + 1.0f));
            }
            else processor.setInputChannelPan(g, 0, p);
        }
        else if (k == "mute") {
            processor.setInputGroupMuted(g, (bool) value);
        }
        else if (k == "pair") {
            processor.setInputGroupStereoPair(g, (bool) value);
        }
        else if ((k == "gate" || k == "comp" || k == "eq" || k == "rev" || k == "dly") && isNumber) {
            // an effect's knob (T-084): its amount, 0 turns it off
            const float amount = jlimit(0.0f, 1.0f, (float) value);
            const bool on = amount > 0.001f;
            if (k == "gate" || k == "comp") {
                SonoAudio::CompressorParams p;
                if (k == "gate") processor.getInputExpanderParams(g, p); else processor.getInputCompressorParams(g, p);
                p.enabled = on;
                if (on) p.mix = amount;
                if (k == "gate") processor.setInputExpanderParams(g, p); else processor.setInputCompressorParams(g, p);
            }
            else if (k == "eq") {
                SonoAudio::ParametricEqParams p;
                processor.getInputEqParams(g, p);
                p.enabled = on;
                if (on) p.mix = amount;
                processor.setInputEqParams(g, p);
            }
            else if (k == "rev") {
                processor.setInputReverbSend(g, on ? amount : 0.0f, true);
            }
            else processor.setInputDelaySend(g, on ? amount : 0.0f);
        }
        sendState();
    }
    else if (type == "instrument") {
        // an Instrument strip (T-086/T-087): level, mute, its menu, its keyboard; or a new strip
        const auto k = message.getProperty("k", var()).toString();
        const var nv = message.getProperty("n", var(0));
        const int n = (nv.isInt() || nv.isInt64() || nv.isDouble()) ? (int) nv : -1;
        if (k == "add") processor.addLiveInstrumentTrack();
        else if (k == "remove") {
            // owner 2026-11-25: remove a strip like adding one; at least one stays (the page asks first
            // when an instrument is loaded)
            if (n >= 0 && n < processor.getNumLiveInstruments() && processor.getNumLiveInstruments() > 1)
                processor.removeLiveInstrumentTrack(n);
        }
        else if (n >= 0 && n < processor.getNumLiveInstruments()) {
            auto & li = processor.getLiveInstrument(n);
            if (k == "gain" && isNumber) li.gain = jlimit(0.0f, 4.0f, (float) value);
            else if (k == "mute") li.muted = (bool) value;
            else if (k == "menu") showInstrumentMenu(n);
            else if (k == "device") showMidiDeviceMenu(n);
        }
        sendState();
    }
    else if (type == "slot") {
        // a strip's VST3 effect slot (T-086): the choice opens as a menu of its own, above the page
        const var iv = message.getProperty("i", var()), sv = message.getProperty("s", var());
        if (!(iv.isInt() || iv.isDouble()) || !(sv.isInt() || sv.isDouble())) return;
        const int g = (int) iv, s = (int) sv;
        if (g < 0 || g >= processor.getInputGroupCount() || s < 0 || s >= InsertChain::numSlots) return;
        showSlotMenu(g, s);
    }
    else if (type == "inputFx") {
        // an effect's pop-up (T-084): its settings, leaving the knob's amount alone
        const var iv = message.getProperty("i", var());
        if (!(iv.isInt() || iv.isDouble())) return;
        const int g = (int) iv;
        if (g < 0 || g >= processor.getInputGroupCount()) return;
        const auto e = message.getProperty("e", var()).toString();
        const var p = message.getProperty("p", var());
        if (!p.isObject()) return;
        auto num = [&p](const char * key, float lo, float hi, float fallback) {
            const var v = p.getProperty(key, var());
            return (v.isInt() || v.isDouble()) ? jlimit(lo, hi, (float) (double) v) : fallback;
        };
        if (e == "gate" || e == "comp") {
            SonoAudio::CompressorParams c;
            if (e == "gate") processor.getInputExpanderParams(g, c); else processor.getInputCompressorParams(g, c);
            c.thresholdDb = num("threshold", -80.0f, 0.0f, c.thresholdDb);
            c.ratio = num("ratio", 1.0f, 20.0f, c.ratio);
            c.attackMs = num("attack", 0.1f, 200.0f, c.attackMs);
            c.releaseMs = num("release", 5.0f, 2000.0f, c.releaseMs);
            if (e == "comp") {
                const var am = p.getProperty("autoMakeup", var());
                if (am.isBool()) c.automakeupGain = (bool) am;
                c.makeupGainDb = num("makeup", 0.0f, 30.0f, c.makeupGainDb);
                processor.setInputCompressorParams(g, c);
            }
            else processor.setInputExpanderParams(g, c);
        }
        else if (e == "eq") {
            SonoAudio::ParametricEqParams q;
            processor.getInputEqParams(g, q);
            q.lowShelfGain = num("lowGain", -18.0f, 18.0f, q.lowShelfGain);
            q.lowShelfFreq = num("lowFreq", 20.0f, 500.0f, q.lowShelfFreq);
            q.para1Gain = num("mid1Gain", -18.0f, 18.0f, q.para1Gain);
            q.para1Freq = num("mid1Freq", 40.0f, 8000.0f, q.para1Freq);
            q.para1Q = num("mid1Q", 0.3f, 10.0f, q.para1Q);
            q.para2Gain = num("mid2Gain", -18.0f, 18.0f, q.para2Gain);
            q.para2Freq = num("mid2Freq", 100.0f, 16000.0f, q.para2Freq);
            q.para2Q = num("mid2Q", 0.3f, 10.0f, q.para2Q);
            q.highShelfGain = num("highGain", -18.0f, 18.0f, q.highShelfGain);
            q.highShelfFreq = num("highFreq", 1000.0f, 18000.0f, q.highShelfFreq);
            processor.setInputEqParams(g, q);
        }
        sendState();
    }
    else if (type == "fxBus") {
        // the shared Reverb or Delay's pop-up (T-084)
        const auto e = message.getProperty("e", var()).toString();
        const var p = message.getProperty("p", var());
        if (!p.isObject()) return;
        auto has = [&p](const char * key) { const var v = p.getProperty(key, var()); return v.isInt() || v.isDouble(); };
        auto get = [&p](const char * key, float lo, float hi) { return jlimit(lo, hi, (float) (double) p.getProperty(key, var())); };
        if (e == "rev") {
            if (has("level")) setParam(SonobusAudioProcessor::paramInputReverbLevel, get("level", 0.0f, 1.0f));
            if (has("size")) setParam(SonobusAudioProcessor::paramInputReverbSize, get("size", 0.0f, 1.0f));
            if (has("damping")) setParam(SonobusAudioProcessor::paramInputReverbDamping, get("damping", 0.0f, 1.0f));
            if (has("preDelay")) setParam(SonobusAudioProcessor::paramInputReverbPreDelay, get("preDelay", 0.0f, 100.0f));
        }
        else if (e == "dly") {
            auto & d = processor.getInputDelay();
            if (has("time")) d.timeMs = get("time", 20.0f, TinCan::Delay::maxTimeMs);
            if (has("repeats")) d.feedback = get("repeats", 0.0f, 0.9f);
            if (has("tone")) d.toneHz = get("tone", 1000.0f, 16000.0f);
            if (has("level")) d.level = get("level", 0.0f, 1.0f);
        }
        sendState();
    }
    else if (type == "outMute") {
        // the Output button and the Output strip's M (T-077): off remembers the level, on brings it back
        float now = getParam(SonobusAudioProcessor::paramWet);
        if ((bool) value) {
            if (now > 0.001f) outputLevel = now;
            setParam(SonobusAudioProcessor::paramWet, 0.0f);
        }
        else setParam(SonobusAudioProcessor::paramWet, now > 0.001f ? now : outputLevel);
    }
    else if (type == "monitorLevel" && isNumber) {
        // the "Hear myself" slider: 0 is off
        float v = jlimit(0.0f, 1.0f, (float) value);
        if (v > 0.001f) monitorLevel = v;
        setParam(SonobusAudioProcessor::paramDry, v);
    }
    else if (type == "recvMute") {
        setParam(SonobusAudioProcessor::paramMainRecvMute, (bool) value ? 1.0f : 0.0f);
    }
    else if (type == "tools" || type == "board" || type == "score") {
        // the lesson bar's buttons: open (docked or in its own window, as last left) or close
        if (isToolOpen(type)) closeTool(type); else openTool(type);
    }
    else if (type == "dock") {
        auto readRect = [&message](const char * key) {
            juce::Rectangle<float> out;
            if (auto * a = message.getProperty(key, var()).getArray(); a != nullptr && a->size() == 4) {
                float v[4];
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    const var & x = (*a)[k];
                    v[k] = (x.isDouble() || x.isInt()) ? (float) x : NAN;
                    ok = ok && std::isfinite(v[k]);
                }
                juce::Rectangle<float> r(v[0], v[1], v[2], v[3]);
                if (ok && r.getWidth() > 0.0f && r.getHeight() > 0.0f) out = r.getIntersection({ 0.0f, 0.0f, 1.0f, 1.0f });
            }
            return out;
        };
        toolsRect = readRect("rect");
        sideRect = readRect("side");
        placeFrame();
    }
    else if (type == "chat") {
        auto text = message.getProperty("text", var()).toString().trim().substring(0, 2000);
        if (text.isEmpty() || processor.getCurrentJoinedGroup().isEmpty()) return;
        SBChatEvent ev;
        ev.from = processor.getCurrentUsername();   // the name we joined with ("Mia 2" after a clash)
        ev.group = processor.getCurrentJoinedGroup();
        ev.message = text;
        processor.sendChatEvent(ev);
        ev.type = SBChatEvent::SelfType;   // the processor only keeps what it receives
        processor.getAllChatEvents().add(ev);
        sendNewChat();
    }
    else if (type == "rec") {
        auto a = message.getProperty("a", var()).toString();
        if (a == "ask" && hooks.recordAsk) hooks.recordAsk();
        if (a == "stop" && hooks.recordStop) hooks.recordStop();
    }
    else if (type == "mark") {
        auto a = message.getProperty("a", var()).toString();
        if (a == "start" && hooks.markStart) hooks.markStart();
        if (a == "save" && hooks.markSave) hooks.markSave(message.getProperty("label", var()).toString().trim().substring(0, 80));
    }
    else if (type == "setup")    { if (hooks.openSetup) hooks.openSetup(); }
    else if (type == "settings") { if (hooks.openSettings) hooks.openSettings(); }
    else if (type == "effects")  { if (info.teacher && hooks.openEffects) hooks.openEffects(); }
    else if (type == "help")     { if (hooks.openHelp) hooks.openHelp(); }
    else if (type == "leave") {
        // owner 2026-11-25: one plain Leave button, then a standard "are you sure" (a native window, so a
        // docked Score or Board can never hide it). The page's own end-of-room leave ("why") goes straight out.
        // the page has already asked "are you sure" in its own pop-up (owner 2026-11-25)
        if (hooks.leave) hooks.leave();
    }
}

void LessonView::handlePeerMessage(int i, const String & kind, const var & value)
{
    using namespace LessonRules;

    if (kind == "mute") {
        processor.setRemotePeerRecvAllow(i, !(bool) value);
    }
    else if (kind == "solo") {
        if (info.teacher) processor.setRemotePeerSoloed(i, (bool) value);
    }
    else if (kind == "quality") {
        Quality q;
        if (!parseQuality(value.toString().toStdString(), q)) return;
        int format = q == Quality::Auto ? -1 : findOpusFormat(bitrateFor(q));
        if (q != Quality::Auto && format < 0) return;
        processor.setRequestRemotePeerSendAudioCodecFormat(i, format);
    }
    else if (kind == "buffer") {
        BufferChoice c;
        if (!parseBufferChoice(value.toString().toStdString(), c)) return;
        processor.setRemotePeerAutoresizeBufferMode(i, (SonobusAudioProcessor::AutoNetBufferMode) bufferModeFor(c));
    }
    else if (kind == "resetBuffer") {
        processor.setRemotePeerBufferTime(i, 0.0f);   // as the app's own reset: starts small and settles again
    }
    else if (kind == "remove") {
        if (info.teacher) processor.removeRemotePeer(i);
    }
}

#else

class LessonView::DockFrame : public Component {};

std::unique_ptr<LessonView> LessonView::create(SonobusAudioProcessor &, const VideoInfo &, const LessonInfo &, Hooks) { return nullptr; }
LessonView::~LessonView() = default;
void LessonView::resized() {}
void LessonView::paint(Graphics &) {}
void LessonView::setPeerVideoId(const String &, const String &) {}
void LessonView::showToast(const String &, int) {}
void LessonView::openTool(const String &) {}
void LessonView::closeTool(const String &) {}
bool LessonView::isToolOpen(const String &) { return false; }
ToolsPanel * LessonView::getTools() const { return nullptr; }
void LessonView::timerCallback() {}
void LessonView::notesArrived(const std::vector<NoteEvent> &) {}

#endif

}
