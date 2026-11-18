// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanTools.h"
#include "TinCanPaths.h"
#include <map>
#include "TinCanTheme.h"
#include "TinCanChords.h"
#include "TinCanPracticeTools.h"
#include "SonobusPluginProcessor.h"

namespace TinCan {

Colour NoteColours::forNote(const NoteState & state, int note)
{
    Colour c;
    if (state.isSounding(note, NoteSource::Midi)) c = midi();
    else if (state.isSounding(note, NoteSource::Local)) c = local();
    else if (state.isSounding(note, NoteSource::Remote)) c = remote();
    else return Colours::transparentBlack;
    return state.isSustainedOnly(note) ? c.withAlpha(0.45f) : c;
}

static bool isBlackKey(int note)
{
    switch (note % 12) { case 1: case 3: case 6: case 8: case 10: return true; default: return false; }
}

//==============================================================================

KeyboardView::KeyboardView(MidiKeyboardState & mouseState, const NoteState & notesToShow)
    : MidiKeyboardComponent(mouseState, MidiKeyboardComponent::horizontalKeyboard), notes(notesToShow)
{
    setScrollButtonsVisible(false);
    setWantsKeyboardFocus(false);  // computer-keyboard notes would clash with the app's own keys
    setColour(keyDownOverlayColourId, Colours::transparentBlack);  // our own colours show notes
    setColour(mouseOverKeyOverlayColourId, Colour(0x30f0a030));
}

void KeyboardView::setNaming(const NoteNaming & newNaming)
{
    naming = newNaming;
    repaint();
}

void KeyboardView::setRangeToFit(int lowNote, int highNote)
{
    fitLow = lowNote;
    fitHigh = highNote;
    resized();
}

void KeyboardView::resized()
{
    int whites = 0;
    for (int n = fitLow; n <= fitHigh; ++n) {
        if (!isBlackKey(n)) ++whites;
    }
    if (whites > 0 && getWidth() > 0) setKeyWidth(getWidth() / (float) whites);
    setAvailableRange(fitLow, fitHigh);
    setLowestVisibleKey(fitLow);
    MidiKeyboardComponent::resized();
}

void KeyboardView::drawWhiteNote(int note, Graphics & g, juce::Rectangle<float> area, bool, bool isOver,
                                 Colour lineColour, Colour textColour)
{
    auto fill = NoteColours::forNote(notes, note);
    if (!fill.isTransparent()) {
        g.setColour(fill);
        g.fillRect(area);
        // both sources at once: a mouse-coloured band at the bottom
        if (notes.isSounding(note, NoteSource::Midi) && notes.isSounding(note, NoteSource::Local)) {
            g.setColour(NoteColours::local());
            g.fillRect(area.withTop(area.getBottom() - area.getHeight() * 0.25f));
        }
        // the other person on the same key as me: their colour along the top
        bool mine = notes.isSounding(note, NoteSource::Midi) || notes.isSounding(note, NoteSource::Local);
        if (mine && notes.isSounding(note, NoteSource::Remote)) {
            g.setColour(NoteColours::remote());
            g.fillRect(area.withHeight(area.getHeight() * 0.25f));
        }
    }
    MidiKeyboardComponent::drawWhiteNote(note, g, area, false, isOver, lineColour,
                                         fill.isTransparent() ? textColour : Colours::black);
}

void KeyboardView::drawBlackNote(int note, Graphics & g, juce::Rectangle<float> area, bool, bool isOver,
                                 Colour noteFillColour)
{
    MidiKeyboardComponent::drawBlackNote(note, g, area, false, isOver, noteFillColour);

    auto fill = NoteColours::forNote(notes, note);
    if (!fill.isTransparent()) {
        g.setColour(fill);
        g.fillRect(area.reduced(area.getWidth() * 0.12f, 0).withTrimmedBottom(area.getWidth() * 0.12f));
    }

    if (naming.allNames && area.getWidth() >= 11.0f) {
        g.setColour(fill.isTransparent() ? Colours::white.withAlpha(0.8f) : Colours::black);
        g.setFont(Font(jmin(11.0f, area.getWidth() * 0.8f)).withHorizontalScale(0.8f));
        g.drawText(naming.name(note), area.withTrimmedBottom(3.0f), Justification::centredBottom, false);
    }
}

String KeyboardView::getWhiteNoteText(int note)
{
    // C keys always carry an octave label; with a transposition the written name is shown
    if (note % 12 == 0) return naming.label(note);
    if (naming.allNames && getKeyWidth() >= 14.0f) return naming.name(note);
    return {};
}

//==============================================================================

FretboardView::FretboardView(const NoteState & notesToShow) : notes(notesToShow)
{
    setOpaque(true);
}

void FretboardView::setInstrument(const std::vector<int> & newStrings, int newFrets)
{
    strings = newStrings;
    frets = jmax(1, newFrets);
    repaint();
}

void FretboardView::setNaming(const NoteNaming & newNaming)
{
    naming = newNaming;
    repaint();
}

juce::Rectangle<float> FretboardView::neckArea() const
{
    // left margin holds the open-string names and open-string notes
    auto area = getLocalBounds().toFloat().withTrimmedLeft(52.0f).withTrimmedRight(10.0f).reduced(0, 14.0f);
    // A real neck's shape: its length to the last fret against the width of its strings
    // (guitar: 648 mm scale, strings about 8.5 mm apart; bass: 864 mm, about 14 mm). Never
    // stretched: as big as fits, centred.
    const bool bass = !strings.empty() && strings.front() < 36;
    const double scale = bass ? 864.0 : 648.0, spacing = bass ? 14.0 : 8.5;
    const double length = scale * (1.0 - std::pow(2.0, -frets / 12.0));
    const float aspect = (float) (length / (spacing * jmax<size_t>(1, strings.size())));
    float w = area.getWidth(), h = area.getHeight();
    if (w / h > aspect) w = h * aspect; else h = w / aspect;
    return juce::Rectangle<float>(w, h).withCentre(area.getCentre());
}

float FretboardView::fretX(int fret) const
{
    // real fret spacing (each fret 2^(-1/12) of the one before), scaled to fit the last fret
    auto neck = neckArea();
    auto pos = [](int f) { return 1.0 - std::pow(2.0, -f / 12.0); };
    return neck.getX() + (float) (pos(fret) / pos(frets)) * neck.getWidth();
}

float FretboardView::noteX(int fret) const
{
    if (fret == 0) return neckArea().getX() - 16.0f;
    return (fretX(fret - 1) + fretX(fret)) * 0.5f;
}

float FretboardView::stringY(int string) const
{
    // highest-pitched string at the top
    auto neck = neckArea();
    int n = (int) strings.size();
    if (n <= 1) return neck.getCentreY();
    int row = n - 1 - string;
    return neck.getY() + neck.getHeight() * (row + 0.5f) / (float) n;
}

int FretboardView::stringAt(float y) const
{
    int best = -1;
    float bestDist = 1e9f;
    for (int s = 0; s < (int) strings.size(); ++s) {
        float d = std::abs(stringY(s) - y);
        if (d < bestDist) { bestDist = d; best = s; }
    }
    float spacing = neckArea().getHeight() / (float) jmax(1, (int) strings.size());
    return bestDist <= spacing * 0.5f ? best : -1;
}

int FretboardView::fretAt(float x) const
{
    auto neck = neckArea();
    if (x < neck.getX()) return x >= neck.getX() - 32.0f ? 0 : -1;
    for (int f = 1; f <= frets; ++f) {
        if (x <= fretX(f)) return f;
    }
    return -1;
}

void FretboardView::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);
    auto neck = neckArea();
    int n = (int) strings.size();
    if (n == 0) return;

    // wood, then inlays
    g.setColour(Colour(0xff3a2c22));
    g.fillRect(neck);
    g.setColour(Colour(0xffd8cfc0).withAlpha(0.55f));
    const int inlays[] = { 3, 5, 7, 9, 15, 17, 19, 21 };
    float dot = jmin(10.0f, neck.getHeight() / (float) n * 0.4f);
    for (int f : inlays) {
        if (f <= frets) g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({ noteX(f), neck.getCentreY() }));
    }
    for (int f = 12; f <= frets; f += 12) {
        g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({ noteX(f), neck.getY() + neck.getHeight() * 0.25f }));
        g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({ noteX(f), neck.getY() + neck.getHeight() * 0.75f }));
    }

    // nut and frets, with numbers under the neck
    g.setColour(Colour(0xffe8e0d0));
    g.fillRect(neck.getX() - 3.0f, neck.getY(), 4.0f, neck.getHeight());
    g.setFont(Font(11.0f));
    for (int f = 1; f <= frets; ++f) {
        float x = fretX(f);
        g.setColour(Colour(0xffb0b0b0));
        g.fillRect(x - 1.0f, neck.getY(), 2.0f, neck.getHeight());
        if (f == 1 || f == 3 || f == 5 || f == 7 || f == 9 || f % 12 == 0 || f == 15 || f == 17 || f == 19 || f == 21) {
            g.setColour(Colour(0x99ffffff));
            g.drawText(String(f), juce::Rectangle<float>(noteX(f) - 12, neck.getBottom() + 1, 24, 12), Justification::centred);
        }
    }

    // strings (thicker for lower strings) and open-string names
    for (int s = 0; s < n; ++s) {
        float y = stringY(s);
        float thick = jmap((float) s, 0.0f, (float) jmax(1, n - 1), 3.0f, 1.0f);
        g.setColour(Colour(0xffcfcfcf));
        g.fillRect(neck.getX(), y - thick * 0.5f, neck.getWidth(), thick);
        g.setColour(Colour(0xccffffff));
        g.setFont(Font(12.0f));
        g.drawText(naming.label(strings[(size_t) s]), juce::Rectangle<float>(neck.getX() - 50.0f, y - 8, 30, 16), Justification::centredLeft);
    }

    // sounding notes, every place they can be played
    float r = jmin(11.0f, neck.getHeight() / (float) n * 0.45f);
    g.setFont(Font(jmin(11.0f, r * 1.1f)).withHorizontalScale(0.85f));
    for (int note = 0; note < 128; ++note) {
        auto fill = NoteColours::forNote(notes, note);
        if (fill.isTransparent()) continue;
        for (auto & p : fretPositions(strings, frets, note)) {
            auto circle = juce::Rectangle<float>(r * 2, r * 2).withCentre({ noteX(p.fret), stringY(p.string) });
            g.setColour(fill);
            g.fillEllipse(circle);
            if (r >= 7.0f) {
                g.setColour(Colours::black);
                g.drawText(naming.name(note), circle, Justification::centred, false);
            }
        }
    }
}

void FretboardView::mouseDown(const MouseEvent & e)
{
    int s = stringAt(e.position.y);
    int f = fretAt(e.position.x);
    if (s < 0 || f < 0) return;
    mouseNote = strings[(size_t) s] + f;
    if (onMousePlay) onMousePlay(mouseNote, true);
}

void FretboardView::mouseUp(const MouseEvent &)
{
    if (mouseNote >= 0 && onMousePlay) onMousePlay(mouseNote, false);
    mouseNote = -1;
}

//==============================================================================

struct RangePreset { const char * name; int low; int keys; };

static const RangePreset rangePresets[] = {
    { "25 keys", 48, 25 },   // C3-C5
    { "37 keys", 48, 37 },   // C3-C6
    { "49 keys", 36, 49 },   // C2-C6
    { "61 keys", 36, 61 },   // C2-C7
    { "88 keys", 21, 88 },   // A0-C8, full piano
};

static constexpr int customTuningId = 1000;

InstrumentPanel::InstrumentPanel(SonobusAudioProcessor & proc, std::function<AudioDeviceManager*()> getDM,
                                 NoteShare & share)
    : processor(proc), getDeviceManager(std::move(getDM)), noteShare(share), keyboard(mouseState, notes), fretboard(notes)
{
    addAndMakeVisible(keyboard);
    addChildComponent(fretboard);
    mouseState.addListener(this);
    fretboard.onMousePlay = [this](int note, bool on) { playLocal(note, on, 1, 0.8f); };

    // the instrument: a circle that opens to the others on hover
    {
        std::vector<IconPicker::Option> opts;
        auto & list = getInstruments();
        for (int i = 0; i < (int) list.size(); ++i) {
            IconPicker::Option o;
            o.id = i;
            const bool keys = list[(size_t) i].kind == InstrumentDef::Keyboard;
            o.icon = keys ? Icons::piano() : Icons::guitar();
            const auto n = list[(size_t) i].tunings.empty() ? 0 : list[(size_t) i].tunings[0].strings.size();
            if (!keys && n != 6) o.badge = String((int) n);   // basses: their string count
            o.tip = TRANS(String(list[(size_t) i].name));
            opts.push_back(o);
        }
        instrumentPicker.setOptions(opts, 0);
        instrumentPicker.onChange = [this](int id) { selectInstrument(id); saveSettings(); };
        addAndMakeVisible(instrumentPicker);
    }

    // piano: how many keys, and octave down/up
    {
        std::vector<IconPicker::Option> opts;
        for (int i = 0; i < (int) std::size(rangePresets); ++i) {
            IconPicker::Option o;
            o.id = i;
            o.text = String(rangePresets[i].keys);
            o.tip = TRANS(rangePresets[i].name);
            opts.push_back(o);
        }
        rangePicker.setOptions(opts, rangeIndex);
        rangePicker.onChange = [this](int id) { selectRange(id); saveSettings(); };
        addAndMakeVisible(rangePicker);
    }
    // what an open picker covers fades out until it closes
    // (set straight, not animated: an animation on a hidden control can leave it see-through
    // when it's shown again for another instrument)
    auto fade = [](std::initializer_list<Component *> list, bool hide) {
        for (auto * c : list) c->setAlpha(hide ? 0.0f : 1.0f);
    };
    instrumentPicker.onOpenChanged = [this, fade](bool o) { fade({ &rangePicker, &octaveDown, &octaveUp, &tuningCircle, &tuningEditor }, o); };
    rangePicker.onOpenChanged = [this, fade](bool o) { fade({ &octaveDown, &octaveUp }, o); };
    octaveDown.setIcon(Icons::chevronLeft());
    octaveUp.setIcon(Icons::chevronRight());
    octaveDown.setTooltip(TRANS("Octave down")); octaveDown.setTitle(TRANS("Octave down"));
    octaveUp.setTooltip(TRANS("Octave up")); octaveUp.setTitle(TRANS("Octave up"));
    octaveDown.onClick = [this]() { lowNote -= 12; updateRange(); saveSettings(); };
    octaveUp.onClick = [this]() { lowNote += 12; updateRange(); saveSettings(); };
    addAndMakeVisible(octaveDown);
    addAndMakeVisible(octaveUp);

    // guitar and bass: a tuning menu; the text field only for a custom tuning
    tuningCircle.setTitle(TRANS("Tuning"));
    tuningCircle.onClick = [this]() { showTuningMenu(); };
    addChildComponent(tuningCircle);
    tuningEditor.setTooltip(TRANS("Open strings from lowest to highest, e.g. D2 A2 D3 G3 B3 E4 (middle C = C4). Press Enter to use it."));
    tuningEditor.setTitle(TRANS("Custom tuning"));
    tuningEditor.onTextChange = [this]() { showTuningValidity(); };  // red while it isn't a tuning
    tuningEditor.onReturnKey = [this]() { applyCustomTuning(); };
    tuningEditor.onFocusLost = [this]() { applyCustomTuning(); };
    addChildComponent(tuningEditor);

    // note names: all names, flats, octave numbering, transposition
    namesCircle.setText("ABC");
    namesCircle.setClickingTogglesState(true);
    namesCircle.setTooltip(TRANS("Name every note, not only the ones played")); namesCircle.setTitle(TRANS("All note names"));
    flatsCircle.setText(String::fromUTF8("\xe2\x99\xad"));   // flat sign
    flatsCircle.setClickingTogglesState(true);
    flatsCircle.setTooltip(TRANS("Name notes with flats (Bb) instead of sharps (A#)")); flatsCircle.setTitle(TRANS("Flats"));
    middleCCircle.setTitle(TRANS("Octave numbering"));
    middleCCircle.onClick = [this]() { naming.middleCOctave = naming.middleCOctave == 4 ? 3 : 4; updateNaming(); saveSettings(); };
    transposeCircle.setTitle(TRANS("Note names for a transposing instrument"));
    transposeCircle.onClick = [this]() { showTransposeMenu(); };
    namesCircle.onClick = [this]() { updateNaming(); saveSettings(); };
    flatsCircle.onClick = [this]() { updateNaming(); saveSettings(); };
    for (auto * c : { &namesCircle, &flatsCircle, &middleCCircle, &transposeCircle }) addAndMakeVisible(c);

    // sharing keys in a lesson (T-074), and the MIDI input
    shareCircle.setIcon(Icons::cast());
    shareCircle.setTitle(TRANS("Share my keys"));
    shareCircle.onClick = [this]() { noteShare.setSharing(!noteShare.isSharing()); updateShareControls(); };
    showCircle.setIcon(Icons::eye());
    showCircle.setTitle(TRANS("Show the other person's keys"));
    showCircle.onClick = [this]() { noteShare.setShowingRemote(!noteShare.isShowingRemote()); updateShareControls(); };
    midiCircle.setIcon(Icons::midi());
    midiCircle.setTitle(TRANS("MIDI input"));
    midiCircle.onClick = [this]() { updateMidiStatus(); };
    for (auto * c : { &shareCircle, &showCircle, &midiCircle }) addAndMakeVisible(c);

    midiStatus.setFont(Font(12.5f));
    midiStatus.setColour(Label::textColourId, Colour(0xb3ffffff));
    addAndMakeVisible(midiStatus);

    chordLabel.setFont(Font(18.0f, Font::bold));
    chordLabel.setColour(Label::textColourId, Colours::white);
    chordLabel.setJustificationType(Justification::centredRight);
    chordLabel.setTitle(TRANS("Chord"));
    addAndMakeVisible(chordLabel);

    lastNoteLabel.setFont(Font(15.0f, Font::bold));
    lastNoteLabel.setColour(Label::textColourId, Colours::white);
    lastNoteLabel.setJustificationType(Justification::centredRight);
    addAndMakeVisible(lastNoteLabel);

    if (auto * dm = getDeviceManager ? getDeviceManager() : nullptr) dm->addChangeListener(this);
    updateMidiStatus();

    // piano with 61 keys the first time; after that, what was chosen last
    loadSettings();

    // MIDI input and the others' keys arrive through the note share (T-074)
    noteShare.addListener(this);

    // development aid: TINCAN_DEMO_NOTES="C4 E4 G4" shows those notes as held on the MIDI input, and
    // shares them when in a lesson, so two copies on one PC can test sharing keys (T-074). After
    // addListener: starting the MIDI tap forgets MIDI notes from before.
    auto demoNotes = parseTuning(SystemStats::getEnvironmentVariable("TINCAN_DEMO_NOTES", {}).toStdString());
    if (!demoNotes.empty()) noteShare.setSharing(true);
    for (int note : demoNotes) {
        NoteEvent ev;
        ev.type = NoteEvent::NoteOn;
        ev.note = (uint8_t) note;
        ev.value = 100;
        notes.apply(ev);
        noteShare.playLocal(ev);   // shared like real playing
        lastNote = note;
    }
    notesChanged();

    updateShareControls();
}

InstrumentPanel::~InstrumentPanel()
{
    noteShare.removeListener(this);
    if (auto * dm = getDeviceManager ? getDeviceManager() : nullptr) dm->removeChangeListener(this);
    mouseState.removeListener(this);
}

static File instrumentSettingsFile() { return TinCan::getDataFolder().getChildFile("instrument.json"); }

void InstrumentPanel::loadSettings()
{
    loading = true;
    var s;
    auto file = instrumentSettingsFile();
    if (file.existsAsFile() && file.getSize() < 64 * 1024) s = JSON::parse(file.loadFileAsString());
    auto num = [&](const char * key, int fallback) { return s.isObject() && s.hasProperty(key) ? (int) s.getProperty(key, fallback) : fallback; };

    naming.allNames = num("names", 0) != 0;
    naming.useFlats = num("flats", 0) != 0;
    naming.middleCOctave = num("middleC", 4) == 3 ? 3 : 4;
    transposeIndex = jlimit(0, (int) getTranspositions().size() - 1, num("transpose", 0));
    namesCircle.setOn(naming.allNames);
    flatsCircle.setOn(naming.useFlats);

    rangeIndex = jlimit(0, (int) std::size(rangePresets) - 1, num("range", 3));
    lowNote = rangePresets[rangeIndex].low;
    numKeys = rangePresets[rangeIndex].keys;
    lowNote = num("low", lowNote);
    rangePicker.setSelected(rangeIndex);

    selectInstrument(jlimit(0, (int) getInstruments().size() - 1, num("instrument", 0)));
    instrumentPicker.setSelected(instrumentIndex);
    if (isFretted()) {
        const int t = num("tuning", 0);
        auto custom = s.isObject() ? s.getProperty("custom", var()).toString() : String();
        if (t < 0 && parseTuning(custom.toStdString()).size() > 0) {
            tuningEditor.setText(custom, dontSendNotification);
            tuningIndex = -1;
            applyCustomTuning();
        }
        else selectTuning(jmax(0, t));
    }
    updateRange();
    updateNaming();
    loading = false;
}

void InstrumentPanel::saveSettings() const
{
    if (loading) return;
    DynamicObject::Ptr s = new DynamicObject();
    s->setProperty("instrument", instrumentIndex);
    s->setProperty("range", rangeIndex);
    s->setProperty("low", lowNote);
    s->setProperty("tuning", tuningIndex);
    if (tuningIndex < 0) s->setProperty("custom", tuningEditor.getText().substring(0, 200));
    s->setProperty("names", naming.allNames ? 1 : 0);
    s->setProperty("flats", naming.useFlats ? 1 : 0);
    s->setProperty("middleC", naming.middleCOctave);
    s->setProperty("transpose", transposeIndex);
    instrumentSettingsFile().replaceWithText(JSON::toString(var(s.get())));
}

bool InstrumentPanel::isFretted() const
{
    return getInstruments()[(size_t) instrumentIndex].kind == InstrumentDef::Fretted;
}

void InstrumentPanel::selectInstrument(int index)
{
    auto & list = getInstruments();
    instrumentIndex = jlimit(0, (int) list.size() - 1, index);
    bool fretted = isFretted();

    // only the view changes: the note state and the MIDI queue carry on
    keyboard.setVisible(!fretted);
    rangePicker.setVisible(!fretted);
    octaveDown.setVisible(!fretted);
    octaveUp.setVisible(!fretted);
    fretboard.setVisible(fretted);
    tuningCircle.setVisible(fretted);
    if (fretted) selectTuning(0);
    else tuningEditor.setVisible(false);
    resized();
}

void InstrumentPanel::selectRange(int presetIndex)
{
    rangeIndex = jlimit(0, (int) std::size(rangePresets) - 1, presetIndex);
    lowNote = rangePresets[rangeIndex].low;
    numKeys = rangePresets[rangeIndex].keys;
    updateRange();
}

void InstrumentPanel::selectTuning(int index)
{
    auto & inst = getInstruments()[(size_t) instrumentIndex];
    if (index < 0 || index >= (int) inst.tunings.size()) {
        // custom: the text field, starting from the strings shown now
        tuningIndex = -1;
        tuningEditor.setVisible(true);
        resized();
        tuningEditor.grabKeyboardFocus();
        tuningEditor.selectAll();
        updateTuningCircle();
        return;
    }
    tuningIndex = index;
    auto & strings = inst.tunings[(size_t) index].strings;
    fretboard.setInstrument(strings, inst.frets);
    tuningEditor.setText(String(formatTuning(strings, naming.useFlats)), dontSendNotification);
    tuningEditor.setVisible(false);
    showTuningValidity();
    updateTuningCircle();
    resized();
}

// the circle shows a short name for the tuning; the menu and its tip the full one
void InstrumentPanel::updateTuningCircle()
{
    auto & inst = getInstruments()[(size_t) instrumentIndex];
    String full = tuningIndex >= 0 && tuningIndex < (int) inst.tunings.size() ? TRANS(String(inst.tunings[(size_t) tuningIndex].name)) : TRANS("Custom tuning");
    String shortName;
    if (tuningIndex < 0) shortName = String::fromUTF8("\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");   // •••
    else {
        auto name = String(inst.tunings[(size_t) tuningIndex].name);
        if (name.startsWith("Standard")) shortName = "Std";
        else if (name.startsWith("Drop")) shortName = "Drop";
        else if (name.startsWith("Half")) shortName = String::fromUTF8("E\xe2\x99\xad");
        else if (name.startsWith("Open ")) shortName = name.substring(5, 6);
        else shortName = name.substring(0, 3);
    }
    tuningCircle.setText(shortName);
    tuningCircle.setTooltip(TRANS("Tuning:") + " " + full);
}

void InstrumentPanel::showTuningMenu()
{
    auto & inst = getInstruments()[(size_t) instrumentIndex];
    PopupMenu m;
    for (int i = 0; i < (int) inst.tunings.size(); ++i) m.addItem(i + 1, TRANS(String(inst.tunings[(size_t) i].name)), true, i == tuningIndex);
    m.addSeparator();
    m.addItem(customTuningId, TRANS("Custom..."), true, tuningIndex < 0);
    SafePointer<InstrumentPanel> safe(this);
    m.showMenuAsync(PopupMenu::Options().withTargetComponent(&tuningCircle), [safe](int r) {
        if (!safe || r == 0) return;
        safe->selectTuning(r == customTuningId ? -1 : r - 1);
        safe->saveSettings();
    });
}

void InstrumentPanel::showTransposeMenu()
{
    auto & list = getTranspositions();
    PopupMenu m;
    for (int i = 0; i < (int) list.size(); ++i) m.addItem(i + 1, TRANS(String(list[(size_t) i].name)), true, i == transposeIndex);
    SafePointer<InstrumentPanel> safe(this);
    m.showMenuAsync(PopupMenu::Options().withTargetComponent(&transposeCircle), [safe](int r) {
        if (!safe || r == 0) return;
        safe->transposeIndex = r - 1;
        safe->updateNaming();
        safe->saveSettings();
    });
}

bool InstrumentPanel::showTuningValidity()
{
    bool valid = !parseTuning(tuningEditor.getText().toStdString()).empty();
    auto colour = valid ? Colours::white : Colour(0xffff6060);
    tuningEditor.setColour(TextEditor::textColourId, colour);
    tuningEditor.applyColourToAllText(colour, true);
    return valid;
}

void InstrumentPanel::applyCustomTuning()
{
    if (!isFretted() || !showTuningValidity()) return;
    auto strings = parseTuning(tuningEditor.getText().toStdString());

    // a preset typed by hand selects that preset; anything else stays custom
    auto & inst = getInstruments()[(size_t) instrumentIndex];
    for (size_t i = 0; i < inst.tunings.size(); ++i) {
        if (inst.tunings[i].strings == strings) { selectTuning((int) i); saveSettings(); return; }
    }
    tuningIndex = -1;
    fretboard.setInstrument(strings, inst.frets);
    updateTuningCircle();
    saveSettings();
}

void InstrumentPanel::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);

    // legend: swatch + text for each kind of note
    struct Item { Colour colour; String text; };
    const Item items[] = {
        { NoteColours::midi(), TRANS("MIDI input") },
        { NoteColours::local(), TRANS("Mouse") },
        { NoteColours::remote(), noteShare.getRemoteName().isNotEmpty() ? noteShare.getRemoteName() : TRANS("Shared keys") },
        { NoteColours::midi().withAlpha(0.45f), TRANS("Held by pedal") },
    };
    Font font(12.0f);
    g.setFont(font);
    auto r = legendArea;
    for (int i = (int) std::size(items) - 1; i >= 0; --i) {
        int tw = font.getStringWidth(items[i].text) + 2;
        auto textArea = r.removeFromRight(tw);
        auto swatch = r.removeFromRight(14).withSizeKeepingCentre(10, 10);
        r.removeFromRight(10);
        g.setColour(items[i].colour);
        g.fillRoundedRectangle(swatch.toFloat(), 2.0f);
        g.setColour(Colour(0x99ffffff));
        g.drawText(items[i].text, textArea, Justification::centredLeft, false);
    }
}

void InstrumentPanel::resized()
{
    auto r = getLocalBounds().reduced(8);
    const int d = IconCircle::size;

    // icons: the instrument and its menus on the left, naming and sharing on the right
    auto row = r.removeFromTop(d + 4);
    auto place = [&](Component & c, int width) { c.setBounds(row.removeFromLeft(width).withSizeKeepingCentre(width, d)); row.removeFromLeft(6); };
    auto placeRight = [&](Component & c) { c.setBounds(row.removeFromRight(d).withSizeKeepingCentre(d, d)); row.removeFromRight(6); };
    // a picker is as wide as when it's open (it draws over its neighbours then), but takes only
    // its closed width in the row
    auto placePicker = [&](IconPicker & p) {
        p.setBounds(row.getX(), row.getY() + (row.getHeight() - d - 4) / 2, p.getOpenWidth(), d + 4);
        row.removeFromLeft(p.getClosedWidth() + 6);
    };
    placeRight(midiCircle);
    placeRight(showCircle);
    placeRight(shareCircle);
    row.removeFromRight(10);
    placeRight(transposeCircle);
    placeRight(middleCCircle);
    placeRight(flatsCircle);
    placeRight(namesCircle);
    placePicker(instrumentPicker);
    row.removeFromLeft(8);
    if (isFretted()) {
        place(tuningCircle, d);
        if (tuningEditor.isVisible()) tuningEditor.setBounds(row.removeFromLeft(jmin(220, jmax(120, row.getWidth() - 8))).withSizeKeepingCentre(jmin(220, jmax(120, row.getWidth() - 8)), 26));
    }
    else {
        placePicker(rangePicker);
        row.removeFromLeft(4);
        place(octaveDown, d);
        place(octaveUp, d);
    }
    // the open pickers draw over everything else
    if (instrumentPicker.isOpen()) instrumentPicker.toFront(false);
    if (rangePicker.isOpen()) rangePicker.toFront(false);

    r.removeFromTop(4);
    auto info = r.removeFromTop(24);
    lastNoteLabel.setBounds(info.removeFromRight(220));
    info.removeFromRight(12);
    chordLabel.setBounds(info.removeFromRight(jmin(320, jmax(0, info.getWidth() / 2))));
    midiStatus.setBounds(info);

    auto status = r.removeFromBottom(20);
    legendArea = status.removeFromRight(jmin(360, status.getWidth()));
    r.removeFromBottom(4);

    // the keyboard keeps a real piano's proportions: as wide as fits, never stretched or squashed
    int whites = 0;
    for (int n = lowNote; n < lowNote + numKeys; ++n) if (!MidiMessage::isMidiNoteBlack(n)) ++whites;
    float keyW = jmin(r.getWidth() / (float) jmax(1, whites), r.getHeight() / whiteKeyHeightPerWidth);
    int kw = (int) std::floor(keyW * whites), kh = (int) std::floor(keyW * whiteKeyHeightPerWidth);
    keyboard.setBounds(juce::Rectangle<int>(kw, kh).withCentre(r.getCentre()).withY(r.getY()));
    fretboard.setBounds(r);
}

void InstrumentPanel::updateRange()
{
    lowNote = jlimit(0, 128 - numKeys, lowNote);
    bool full = numKeys >= 88;
    octaveDown.setEnabled(!full && lowNote - 12 >= 0);
    octaveUp.setEnabled(!full && lowNote + 12 + numKeys <= 128);
    resized();   // the keyboard's size depends on the number of keys
    keyboard.setRangeToFit(lowNote, lowNote + numKeys - 1);
}

void InstrumentPanel::updateNaming()
{
    auto & transpositions = getTranspositions();
    transposeIndex = jlimit(0, (int) transpositions.size() - 1, transposeIndex);
    naming.allNames = namesCircle.getToggleState();
    naming.useFlats = flatsCircle.getToggleState();
    naming.transpose = transpositions[(size_t) transposeIndex].semitones;
    middleCCircle.setText(naming.middleCOctave == 3 ? "C3" : "C4");
    middleCCircle.setTooltip(TRANS("Octave numbering: middle C is") + " " + (naming.middleCOctave == 3 ? String("C3") : String("C4")) + ". " + TRANS("Click to change."));
    // the transposition: a short name in the circle, on (bright) when not at concert pitch
    static const std::map<int, String> shortNames = { { 0, "C" }, { 2, String::fromUTF8("B\xe2\x99\xad") }, { 14, String::fromUTF8("B\xe2\x99\xad" "8") },
                                                      { 9, String::fromUTF8("E\xe2\x99\xad") }, { 21, String::fromUTF8("E\xe2\x99\xad" "8") },
                                                      { 7, "F" }, { 3, "A" }, { 12, "8va" } };
    auto it = shortNames.find(naming.transpose);
    transposeCircle.setText(it != shortNames.end() ? it->second : String(naming.transpose));
    transposeCircle.setOn(naming.transpose != 0);
    transposeCircle.setTooltip(TRANS("Note names:") + " " + TRANS(String(transpositions[(size_t) transposeIndex].name)));
    keyboard.setNaming(naming);
    fretboard.setNaming(naming);
    if (isFretted()) {
        // re-spell the tuning text with sharps or flats (it stays at concert pitch, middle C = C4)
        auto strings = parseTuning(tuningEditor.getText().toStdString());
        if (!strings.empty()) tuningEditor.setText(String(formatTuning(strings, naming.useFlats)), dontSendNotification);
    }
    notesChanged();
}

void InstrumentPanel::updateMidiStatus()
{
    auto * dm = getDeviceManager ? getDeviceManager() : nullptr;
    String text;
    bool on = false;
    if (dm == nullptr) {
        text = TRANS("Notes come from the MIDI sent to this plugin's track.");
        on = true;
    }
    else {
        StringArray names;
        for (auto & d : MidiInput::getAvailableDevices()) {
            if (dm->isMidiInputDeviceEnabled(d.identifier)) names.add(d.name);
        }
        on = !names.isEmpty();
        text = on ? names.joinIntoString(", ") : TRANS("No MIDI input on. Turn one on in Settings > Audio, or click the instrument.");
    }
    // the socket is bright red while a keyboard is connected; the text says which
    midiCircle.setOn(on);
    midiCircle.setTooltip(on ? TRANS("MIDI input:") + " " + text : text);
    midiStatus.setText(on ? TRANS("MIDI:") + " " + text : text, dontSendNotification);
}

void InstrumentPanel::notesChanged()
{
    if (isFretted()) fretboard.repaint();
    else keyboard.repaint();

    // chord names for the held keys (mine: MIDI and mouse together), and the other person's
    std::vector<int> mine, theirs;
    for (int n = 0; n < 128; ++n) {
        if (notes.isSounding(n, NoteSource::Midi) || notes.isSounding(n, NoteSource::Local)) mine.push_back(n);
        if (notes.isSounding(n, NoteSource::Remote)) theirs.push_back(n);
    }
    auto myChord = String(chordName(identifyChord(mine), naming.useFlats, naming.transpose));
    auto theirChord = String(chordName(identifyChord(theirs), naming.useFlats, naming.transpose));
    String chordText = myChord;
    if (theirChord.isNotEmpty()) {
        auto who = noteShare.getRemoteName().isNotEmpty() ? noteShare.getRemoteName() : TRANS("Shared");
        chordText = (myChord.isNotEmpty() ? myChord + "   " : String()) + who + ": " + theirChord;
    }
    if (chordLabel.getText() != chordText) chordLabel.setText(chordText, dontSendNotification);

    // the last note played, with written and concert pitch told apart when transposing
    if (lastNote < 0) {
        lastNoteLabel.setText({}, dontSendNotification);
    }
    else if (naming.transpose == 0) {
        lastNoteLabel.setText(naming.label(lastNote), dontSendNotification);
    }
    else {
        NoteNaming concert = naming;
        concert.transpose = 0;
        lastNoteLabel.setText(TRANS("Written") + " " + naming.label(lastNote) + "   " + TRANS("Concert") + " " + concert.label(lastNote),
                              dontSendNotification);
    }
}

void InstrumentPanel::changeListenerCallback(ChangeBroadcaster *)
{
    // device settings changed: notes held on a device that went away would stay lit
    NoteEvent ev;
    ev.type = NoteEvent::Reset;
    ev.source = NoteSource::Midi;
    notes.apply(ev);
    notesChanged();
    updateMidiStatus();
}

void InstrumentPanel::notesArrived(const std::vector<NoteEvent> & events)
{
    for (auto & ev : events) {
        notes.apply(ev);
        if (ev.type == NoteEvent::NoteOn && ev.source != NoteSource::Remote) lastNote = ev.note;
    }
    notesChanged();
}

void InstrumentPanel::noteShareChanged()
{
    updateShareControls();
    repaint(legendArea);
}

void InstrumentPanel::updateShareControls()
{
    bool canShare = !noteShare.canShare || noteShare.canShare();
    shareCircle.setOn(noteShare.isSharing());
    shareCircle.setEnabled(canShare || noteShare.isSharing());
    shareCircle.setTooltip(!canShare && !noteShare.isSharing() ? TRANS("Join a lesson to share your keys")
                           : noteShare.isSharing() ? TRANS("Sharing your keys: the others see which keys you play. Click to stop.")
                                                   : TRANS("Share my keys: light up your keys on the others' screens while you play. They hear you as usual; this only shows which keys."));

    showCircle.setOn(noteShare.isShowingRemote());
    auto who = noteShare.getRemoteName();
    String text = who.isNotEmpty() ? TRANS("Show") + " " + who + TRANS("'s keys") : TRANS("Show the other person's keys");
    if (noteShare.isRemoteSharing() && !noteShare.isShowingRemote()) text << " (" << TRANS("sharing now") << ")";
    showCircle.setTooltip(text);
}

void InstrumentPanel::playLocal(int note, bool on, int channel, float velocity)
{
    NoteEvent ev;
    ev.type = on ? NoteEvent::NoteOn : NoteEvent::NoteOff;
    ev.channel = (uint8_t) jlimit(0, 15, channel - 1);
    ev.note = (uint8_t) jlimit(0, 127, note);
    ev.value = (uint8_t) jlimit(1, 127, roundToInt(velocity * 127.0f));
    ev.source = NoteSource::Local;
    notes.apply(ev);
    noteShare.playLocal(ev);   // shared too, when sharing is on
    if (on) lastNote = ev.note;
    notesChanged();
}

void InstrumentPanel::handleNoteOn(MidiKeyboardState *, int channel, int note, float velocity)
{
    playLocal(note, true, channel, velocity);
}

void InstrumentPanel::handleNoteOff(MidiKeyboardState *, int channel, int note, float)
{
    playLocal(note, false, channel, 0.0f);
}

//==============================================================================

// Workspace tab (T-037): the board itself needs room, so it has a window of its own
class WorkspacePanel : public Component
{
public:
    explicit WorkspacePanel(std::function<void()> open)
    {
        info.setText(TRANS("A board for lesson material: open images, add whiteboards, and draw, highlight, "
                           "point with arrows or write on them. Files stay on this computer; for now only you see the board."),
                     dontSendNotification);
        info.setFont(Font(14.0f));
        info.setJustificationType(Justification::topLeft);
        addAndMakeVisible(info);
        openButton.onClick = open;
        addAndMakeVisible(openButton);
    }

    void paint(Graphics & g) override { g.fillAll(TinCanTheme::surface); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        openButton.setBounds(r.removeFromTop(32).removeFromLeft(200));
        r.removeFromTop(10);
        info.setBounds(r.removeFromTop(60));
    }

private:
    Label info;
    TextButton openButton { TRANS("Open the workspace") };
};

ToolsPanel::ToolsPanel(SonobusAudioProcessor & processor, std::function<AudioDeviceManager*()> getDeviceManager,
                       NoteShare & noteShare, std::function<void()> openWorkspace, SummaryHooks summaryHooks)
{
    tabs = std::make_unique<TabbedComponent>(TabbedButtonBar::TabsAtTop);
    tabs->setTabBarDepth(28);
    tabs->addTab(TRANS("Tuner"), TinCanTheme::surface, new TunerPanel(processor), true);
    tabs->addTab(TRANS("Metronome"), TinCanTheme::surface, new MetronomePanel(processor), true);
    tabs->addTab(TRANS("Instrument"), TinCanTheme::surface, new InstrumentPanel(processor, getDeviceManager, noteShare), true);
    if (openWorkspace)
        tabs->addTab(TRANS("Workspace"), TinCanTheme::surface, new WorkspacePanel(std::move(openWorkspace)), true);
    summaries = new SummariesPanel(std::move(summaryHooks));
    tabs->addTab(TRANS("Summaries"), TinCanTheme::surface, summaries, true);
    addAndMakeVisible(tabs.get());
    // development aid (see TINCAN_DEMO_NOTES in InstrumentPanel): start on the instrument
    if (SystemStats::getEnvironmentVariable("TINCAN_DEMO_NOTES", {}).isNotEmpty()) tabs->setCurrentTabIndex(2);
}

ToolsPanel::~ToolsPanel() = default;

void ToolsPanel::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);
}

void ToolsPanel::resized()
{
    tabs->setBounds(getLocalBounds());
}

void ToolsPanel::showSummary(const File & summaryFile)
{
    tabs->setCurrentTabIndex(tabs->getNumTabs() - 1);
    summaries->refresh(summaryFile);
}

void ToolsPanel::refreshSummaries()
{
    summaries->refresh();
}

ToolsWindow::ToolsWindow(SonobusAudioProcessor & processor, std::function<AudioDeviceManager*()> getDeviceManager,
                         NoteShare & noteShare, std::function<void()> openWorkspace, SummaryHooks summaryHooks,
                         std::function<void()> onCloseFunc)
    : DocumentWindow(TRANS("Teaching tools"), TinCanTheme::surface, DocumentWindow::allButtons),
      onClose(std::move(onCloseFunc))
{
    panel = new ToolsPanel(processor, std::move(getDeviceManager), noteShare, std::move(openWorkspace), std::move(summaryHooks));

    setUsingNativeTitleBar(true);
    setContentOwned(panel, false);
    setResizable(true, false);
    setResizeLimits(640, 280, 4000, 1200);
    centreWithSize(960, 340);
    setVisible(true);
}

ToolsWindow::~ToolsWindow()
{
    clearContentComponent();
}

void ToolsWindow::closeButtonPressed()
{
    if (onClose) onClose();
}

void ToolsWindow::showSummary(const File & summaryFile)
{
    panel->showSummary(summaryFile);
    toFront(true);
}

void ToolsWindow::refreshSummaries()
{
    panel->refreshSummaries();
}

}
