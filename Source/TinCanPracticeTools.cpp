// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanPracticeTools.h"
#include "TinCanTheme.h"
#include "TinCanNotes.h"
#include "SonobusPluginProcessor.h"

namespace TinCan {

static const Colour panelBackground(TinCanTheme::surface);
static const Colour accent(TinCanTheme::stringLight);
static const Colour accentBar(0xfff0a030);
static const Colour inTune(0xff5ccf72);

//==============================================================================

void BeatIndicator::update(double pos, int bpb, int sub, double countInRemaining, bool isRunning)
{
    position = pos;
    beatsPerBar = jmax(1, bpb);
    subdivision = jmax(1, sub);
    countIn = countInRemaining > 0.0;
    running = isRunning;
    repaint();
}

void BeatIndicator::paint(Graphics & g)
{
    auto r = getLocalBounds().toFloat();
    int beat = (int) std::floor(position);
    double frac = position - beat;
    int inBar = ((beat % beatsPerBar) + beatsPerBar) % beatsPerBar;

    float size = jmin(r.getHeight() * 0.7f, (r.getWidth() - 8.0f * beatsPerBar) / beatsPerBar);
    size = jmax(8.0f, size);
    float total = beatsPerBar * size + (beatsPerBar - 1) * 8.0f;
    float x = r.getCentreX() - total * 0.5f;
    float y = r.getY() + (r.getHeight() - size) * 0.5f - 8.0f;

    for (int b = 0; b < beatsPerBar; ++b) {
        auto circle = juce::Rectangle<float>(x + b * (size + 8.0f), y, size, size);
        bool current = running && b == inBar;
        Colour on = (b == 0 && beatsPerBar > 1) || countIn ? accentBar : accent;
        // the current beat flashes and fades over the beat
        float glow = current ? (float) jlimit(0.25, 1.0, 1.0 - frac * 0.75) : 0.0f;
        g.setColour(current ? on.withAlpha(glow) : TinCanTheme::line);
        g.fillEllipse(circle);
        g.setColour(Colour(0x55ffffff));
        g.drawEllipse(circle, 1.0f);
        g.setColour(current ? Colours::black : Colour(0x99ffffff));
        g.setFont(Font(jmin(22.0f, size * 0.45f), Font::bold));
        g.drawText(String(b + 1), circle, Justification::centred);
    }

    // subdivision ticks under the beats
    if (subdivision > 1 && running) {
        int subIndex = (int) std::floor(frac * subdivision);
        float sx = r.getCentreX() - (subdivision * 14.0f) * 0.5f;
        for (int s = 0; s < subdivision; ++s) {
            g.setColour(s == subIndex ? accent : Colour(0xff444444));
            g.fillRoundedRectangle(sx + s * 14.0f, y + size + 8.0f, 10.0f, 5.0f, 2.0f);
        }
    }

    if (countIn && running) {
        g.setColour(accentBar);
        g.setFont(Font(14.0f, Font::bold));
        g.drawText(TRANS("Count-in"), r.removeFromBottom(16.0f), Justification::centred);
    }
}

//==============================================================================

static void setUpCircle(IconCircle & c, Icons::Icon icon, const String & tip)
{
    c.setIcon(std::move(icon));
    c.setTooltip(tip);
    c.setTitle(tip);
}

MetronomePanel::MetronomePanel(SonobusAudioProcessor & proc) : processor(proc)
{
    auto & state = processor.getValueTreeState();

    // start and stop: bright red while it runs
    setUpCircle(playCircle, Icons::play(), TRANS("Start the metronome. The clicks follow the audio clock, so they stay steady"));
    playCircle.setClickingTogglesState(true);
    playCircle.onStateChange = [this]() {
        const bool on = playCircle.getToggleState();
        playCircle.setIcon(on ? Icons::stop() : Icons::play());
        playCircle.setTooltip(on ? TRANS("Stop the metronome") : TRANS("Start the metronome. The clicks follow the audio clock, so they stay steady"));
    };
    enableAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(state, SonobusAudioProcessor::paramMetEnabled, playCircle);

    // tempo: slower, the number (drag it), faster, tap
    setUpCircle(slowerCircle, Icons::minus(), TRANS("Slower"));
    setUpCircle(fasterCircle, Icons::plus(), TRANS("Faster"));
    slowerCircle.onClick = [this]() { setTempo(roundToInt(tempoSlider.getValue()) - 1); };
    fasterCircle.onClick = [this]() { setTempo(roundToInt(tempoSlider.getValue()) + 1); };
    tempoSlider.setTitle(TRANS("Tempo"));
    tempoSlider.setTooltip(TRANS("Tempo in beats per minute: drag, or use the wheel"));
    tempoSlider.setColour(Slider::trackColourId, TinCanTheme::string.withAlpha(0.35f));
    tempoSlider.setColour(Slider::backgroundColourId, TinCanTheme::raised);
    tempoSlider.setColour(Slider::textBoxTextColourId, TinCanTheme::text);
    tempoAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(state, SonobusAudioProcessor::paramMetTempo, tempoSlider);
    tempoSlider.setNumDecimalPlacesToDisplay(0);
    setUpCircle(tapCircle, Icons::tap(), TRANS("Tap a few times in time to set the tempo"));
    tapCircle.onClick = [this]() { tapTempo(); };

    // beats in a bar
    std::vector<IconPicker::Option> beats;
    for (int i = 1; i <= 12; ++i) beats.push_back({ i, {}, String(i), {}, i == 1 ? TRANS("1 beat in a bar: no accent") : String(i) + " " + TRANS("beats in a bar") });
    beatsPicker.setOptions(std::move(beats), 4);
    beatsPicker.onChange = [this](int id) { setIntParam(SonobusAudioProcessor::paramMetBeatsPerBar, id); };

    // clicks per beat
    subdivisionPicker.setOptions({ { 1, Icons::quarter(), {}, {}, TRANS("One click per beat") },
                                   { 2, Icons::eighths(), {}, {}, TRANS("Two clicks per beat: eighths") },
                                   { 3, Icons::triplet(), {}, {}, TRANS("Three clicks per beat: triplets") },
                                   { 4, Icons::sixteenths(), {}, {}, TRANS("Four clicks per beat: sixteenths") } }, 1);
    subdivisionPicker.onChange = [this](int id) { setIntParam(SonobusAudioProcessor::paramMetSubdivision, id); };

    // count-in: bars of accented clicks when it starts
    countInPicker.setOptions({ { 1, Icons::metronome(), {}, "0", TRANS("No count-in") },
                               { 2, Icons::metronome(), {}, "1", TRANS("Count in one bar when it starts") },
                               { 3, Icons::metronome(), {}, "2", TRANS("Count in two bars when it starts") } }, 1);
    countInPicker.onChange = [this](int id) { setIntParam(SonobusAudioProcessor::paramMetCountIn, id - 1); };

    // every choice shown (owner 2026-11-26)
    subdivisionPicker.setAlwaysOpen(true);
    countInPicker.setAlwaysOpen(true);
    setUpCircle(beatsDown, Icons::minus(), TRANS("Fewer beats in a bar"));
    setUpCircle(beatsUp, Icons::plus(), TRANS("More beats in a bar"));
    beatsDown.onClick = [this]() { setIntParam(SonobusAudioProcessor::paramMetBeatsPerBar, jmax(1, getIntParam(SonobusAudioProcessor::paramMetBeatsPerBar) - 1)); timerCallback(); };
    beatsUp.onClick = [this]() { setIntParam(SonobusAudioProcessor::paramMetBeatsPerBar, jmin(12, getIntParam(SonobusAudioProcessor::paramMetBeatsPerBar) + 1)); timerCallback(); };
    beatsLabel.setJustificationType(Justification::centred);
    beatsLabel.setFont(Font(15.0f, Font::bold));
    beatsLabel.setColour(Label::textColourId, TinCanTheme::text);
    beatsLabel.setTooltip(TRANS("Beats in a bar"));

    // click volume, and whether the lesson hears it
    setUpCircle(volumeIcon, Icons::volume(), TRANS("Click volume"));
    volumeIcon.setInterceptsMouseClicks(false, false);
    volumeSlider.setTitle(TRANS("Click volume"));
    volumeSlider.setTooltip(TRANS("Click volume"));
    volumeSlider.setPopupDisplayEnabled(true, true, this);
    volumeAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(state, SonobusAudioProcessor::paramMetGain, volumeSlider);
    setUpCircle(sendCircle, Icons::cast(), TRANS("Let the other person hear the clicks (off: only you hear them)"));
    sendCircle.setClickingTogglesState(true);
    sendAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(state, SonobusAudioProcessor::paramSendMetAudio, sendCircle);

    for (Component * c : std::initializer_list<Component *> { &playCircle, &slowerCircle, &tempoSlider, &fasterCircle, &tapCircle,
                                                              &beatsDown, &beatsLabel, &beatsUp, &subdivisionPicker, &countInPicker, &volumeIcon, &volumeSlider, &sendCircle, &indicator })
        addAndMakeVisible(c);
    playCircle.onStateChange();
    timerCallback();
}

void MetronomePanel::setTempo(int bpm)
{
    if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetTempo))
        p->setValueNotifyingHost(p->convertTo0to1((float) jlimit(10, 400, bpm)));
}

MetronomePanel::~MetronomePanel()
{
    stopTimer();
}

void MetronomePanel::visibilityChanged()
{
    if (isShowing()) startTimerHz(30);
    else stopTimer();
}

void MetronomePanel::setIntParam(const String & id, int value)
{
    if (auto * p = processor.getValueTreeState().getParameter(id)) {
        p->setValueNotifyingHost(p->convertTo0to1((float) value));
    }
}

int MetronomePanel::getIntParam(const String & id) const
{
    if (auto * v = processor.getValueTreeState().getRawParameterValue(id)) return roundToInt(v->load());
    return 0;
}

void MetronomePanel::tapTempo()
{
    double now = Time::getMillisecondCounterHiRes();
    if (!taps.isEmpty() && now - taps.getLast() > 2000.0) taps.clear();  // a new run of taps
    taps.add(now);
    if (taps.size() > 6) taps.remove(0);
    if (taps.size() < 3) return;
    double avg = (taps.getLast() - taps.getFirst()) / (taps.size() - 1);
    if (auto * p = processor.getValueTreeState().getParameter(SonobusAudioProcessor::paramMetTempo)) {
        double bpm = jlimit(10.0, 400.0, 60000.0 / avg);
        p->setValueNotifyingHost(p->convertTo0to1((float) std::round(bpm)));
    }
}

void MetronomePanel::timerCallback()
{
    int bpb = getIntParam(SonobusAudioProcessor::paramMetBeatsPerBar);
    int sub = getIntParam(SonobusAudioProcessor::paramMetSubdivision);
    int countIn = getIntParam(SonobusAudioProcessor::paramMetCountIn);
    if (beatsPicker.getSelected() != bpb) beatsPicker.setSelected(bpb);
    const auto bpbText = String(bpb) + "/" + TRANS("bar");
    if (beatsLabel.getText() != bpbText) beatsLabel.setText(bpbText, dontSendNotification);
    if (subdivisionPicker.getSelected() != sub) subdivisionPicker.setSelected(sub);
    if (countInPicker.getSelected() != countIn + 1) countInPicker.setSelected(countIn + 1);

    bool running = playCircle.getToggleState();
    indicator.update(processor.getMetronomeBeatPosition(), bpb, sub, processor.getMetronomeCountInRemaining(), running);
}

void MetronomePanel::paint(Graphics & g)
{
    g.fillAll(panelBackground);
}

void MetronomePanel::resized()
{
    auto r = getLocalBounds().reduced(8);
    const int d = IconCircle::size;
    auto row = ToolRail::row(*this, r, 8);
    row = row.withSizeKeepingCentre(row.getWidth(), d + 4);
    auto place = [&](Component & c, int w) { c.setBounds(row.removeFromLeft(w).withSizeKeepingCentre(w, w == d ? d : 30)); row.removeFromLeft(6); };
    auto placePicker = [&](IconPicker & p) {
        p.setBounds(row.getX(), row.getY(), p.getOpenWidth(), d + 4);
        row.removeFromLeft(p.getClosedWidth() + 4);
    };
    place(playCircle, d);
    row.removeFromLeft(10);
    place(slowerCircle, d);
    place(tempoSlider, 86);
    place(fasterCircle, d);
    place(tapCircle, d);
    row.removeFromLeft(10);
    place(beatsDown, d);
    place(beatsLabel, 50);
    place(beatsUp, d);
    row.removeFromLeft(10);
    placePicker(subdivisionPicker);
    placePicker(countInPicker);
    row.removeFromLeft(10);
    place(volumeIcon, d);
    place(volumeSlider, jlimit(80, 160, row.getWidth() - d - 12));
    place(sendCircle, d);
    // an open picker draws over the icons after it
    for (auto * p : { &beatsPicker, &subdivisionPicker, &countInPicker }) if (p->isOpen()) p->toFront(false);

    indicator.setBounds(r.reduced(0, 4));
}

//==============================================================================

void TunerDisplay::setResult(const PitchResult & newResult, double a4, bool flats)
{
    result = newResult;
    useFlats = flats;
    if (result.status == PitchResult::Pitched) {
        note = frequencyToNote(result.frequency, a4);
        double cents = (note - std::round(note)) * 100.0;
        // smooth the needle a little so it doesn't jitter, but follow big moves at once
        shownCents = std::abs(cents - shownCents) > 20.0 ? cents : shownCents * 0.6 + cents * 0.4;
    }
    repaint();
}

void TunerDisplay::paint(Graphics & g)
{
    auto r = getLocalBounds().toFloat().reduced(4.0f);
    bool pitched = result.status == PitchResult::Pitched;
    int nearest = (int) std::round(note);
    bool tuned = pitched && std::abs(shownCents) <= 5.0;
    Colour state = !pitched ? Colour(0xff666666) : tuned ? inTune : (shownCents > 0 ? accentBar : accent);

    // note name
    auto top = r.removeFromTop(r.getHeight() * 0.48f);
    g.setColour(pitched ? Colours::white : Colour(0xff555555));
    g.setFont(Font(jmin(64.0f, top.getHeight() * 0.9f), Font::bold));
    String name = pitched ? String(noteName(nearest, useFlats)) : String("-");
    g.drawText(name, top.withTrimmedRight(top.getWidth() * 0.5f - 6.0f), Justification::centredRight);
    g.setFont(Font(jmin(28.0f, top.getHeight() * 0.4f)));
    g.drawText(pitched ? String(noteOctave(nearest)) : String(), top.withTrimmedLeft(top.getWidth() * 0.5f + 2.0f).withTrimmedBottom(top.getHeight() * 0.18f),
               Justification::bottomLeft);

    // cents scale with a needle
    auto meter = r.removeFromTop(r.getHeight() * 0.55f).reduced(20.0f, 6.0f);
    g.setColour(TinCanTheme::raised);
    g.fillRoundedRectangle(meter, 6.0f);
    auto centreX = meter.getCentreX();
    g.setColour(inTune.withAlpha(0.25f));
    g.fillRect(juce::Rectangle<float>(meter.getWidth() * 0.1f, meter.getHeight()).withCentre(meter.getCentre()));  // +-5 cents
    g.setFont(Font(11.0f));
    for (int c = -50; c <= 50; c += 10) {
        float x = centreX + meter.getWidth() * 0.5f * c / 50.0f;
        g.setColour(Colour(0x66ffffff));
        g.fillRect(x - 0.5f, meter.getY(), 1.0f, c == 0 ? meter.getHeight() : meter.getHeight() * 0.3f);
    }
    g.setColour(Colour(0x99ffffff));
    g.drawText(TRANS("flat"), meter.withTrimmedLeft(6.0f), Justification::bottomLeft);
    g.drawText(TRANS("sharp"), meter.withTrimmedRight(6.0f), Justification::bottomRight);
    if (pitched) {
        float x = centreX + meter.getWidth() * 0.5f * (float) jlimit(-50.0, 50.0, shownCents) / 50.0f;
        g.setColour(state);
        g.fillRoundedRectangle(x - 3.0f, meter.getY() - 4.0f, 6.0f, meter.getHeight() + 8.0f, 3.0f);
    }

    // status line
    String status;
    switch (result.status) {
        case PitchResult::NoSignal: status = TRANS("No sound on this input"); break;
        case PitchResult::TooQuiet: status = TRANS("Too quiet: play louder or move closer"); break;
        case PitchResult::Unclear:  status = TRANS("Can't hear a clear note"); break;
        case PitchResult::Pitched: {
            int cents = roundToInt(shownCents);
            String hz = String(result.frequency, 1) + " Hz   " + (cents > 0 ? "+" : "") + String(cents) + " " + TRANS("cents");
            status = (tuned ? TRANS("In tune") : cents > 0 ? TRANS("Sharp: tune down") : TRANS("Flat: tune up")) + "    " + hz;
            break;
        }
    }
    g.setColour(state == Colour(0xff666666) ? Colour(0xaaffffff) : state);
    g.setFont(Font(16.0f, Font::bold));
    g.drawText(status, r, Justification::centred);
}

//==============================================================================

TunerPanel::TunerPanel(SonobusAudioProcessor & proc) : Thread("CanDuet tuner"), processor(proc)
{
    // which input it listens to: a microphone with the input's number
    inputPicker.onChange = [this](int id) { if (isThreadRunning()) processor.setTunerChannel(id - 1); };
    inputPicker.onOpenChanged = [this](bool open) { if (open) inputPicker.toFront(false); };
    addAndMakeVisible(inputPicker);

    // the reference A: lower, the number, higher
    setUpCircle(refDown, Icons::minus(), TRANS("Lower the reference A by 1 Hz"));
    setUpCircle(refUp, Icons::plus(), TRANS("Raise the reference A by 1 Hz"));
    refDown.onClick = [this]() { setReference(reference - 1.0); };
    refUp.onClick = [this]() { setReference(reference + 1.0); };
    refLabel.setJustificationType(Justification::centred);
    refLabel.setFont(Font(15.0f, Font::bold));
    refLabel.setColour(Label::textColourId, TinCanTheme::text);
    refLabel.setTooltip(TRANS("Reference pitch for A4 (usually 440 Hz). Double-click for 440"));
    refLabel.addMouseListener(this, false);
    for (auto * c : std::initializer_list<Component *> { &refDown, &refLabel, &refUp }) addAndMakeVisible(c);
    setReference(440.0);

    // note names with sharps or flats
    flatsCircle.setText(String::fromUTF8("\xe2\x99\xad"));
    flatsCircle.setTooltip(TRANS("Name notes with flats"));
    flatsCircle.setTitle(TRANS("Name notes with flats"));
    flatsCircle.setClickingTogglesState(true);
    addAndMakeVisible(flatsCircle);

    display.setTooltip(TRANS("Listens to your own input only. The tuner makes no sound and sends nothing to the lesson."));
    addAndMakeVisible(display);

    updateInputs();
}

void TunerPanel::setReference(double hz)
{
    reference = jlimit(415.0, 466.0, std::round(hz));
    refLabel.setText("A " + String((int) reference), dontSendNotification);
}

void TunerPanel::mouseDoubleClick(const MouseEvent & e)
{
    if (e.eventComponent == &refLabel) setReference(440.0);
}

TunerPanel::~TunerPanel()
{
    stopListening();
}

void TunerPanel::updateInputs()
{
    int n = processor.getTotalNumInputChannels();
    if (n == numInputs) return;
    numInputs = n;
    const int sel = jmin(getInput(), jmax(1, n));
    std::vector<IconPicker::Option> inputs;
    for (int i = 1; i <= jmax(1, n); ++i)
        inputs.push_back({ i, Icons::mic(), {}, String(i), TRANS("Listen to input") + " " + String(i) });
    inputPicker.setOptions(std::move(inputs), sel);
    inputPicker.setAlwaysOpen(true);   // owner 2026-11-26: every input shown
    resized();
    if (isThreadRunning() && n > 0) processor.setTunerChannel(getInput() - 1);
}

void TunerPanel::visibilityChanged()
{
    if (isShowing()) startListening();
    else stopListening();
}

void TunerPanel::startListening()
{
    if (isThreadRunning()) return;
    updateInputs();
    processor.setTunerChannel(getInput() - 1);
    startThread(Thread::Priority::low);  // the tuner never competes with audio or the UI
    startTimerHz(20);
}

void TunerPanel::stopListening()
{
    stopTimer();
    processor.setTunerChannel(-1);
    stopThread(1000);
}

void TunerPanel::run()
{
    PitchDetector detector;
    std::vector<float> window;
    double preparedRate = 0.0;

    while (!threadShouldExit()) {
        wait(50);
        double rate = processor.getSampleRate();
        if (rate <= 0.0) continue;
        if (rate != preparedRate) {
            // first run or the audio device changed: size the window for ~85 ms
            int size = jmax(1024, roundToInt(4096 * rate / 48000.0));
            detector.prepare(rate, size);
            window.assign((size_t) size, 0.0f);
            preparedRate = rate;
        }
        PitchResult r;
        if (processor.readTunerSamples(window.data(), (int) window.size())) {
            r = detector.analyse(window.data());
        }
        const SpinLock::ScopedLockType sl(resultLock);
        latest = r;
        hasNew = true;
    }
}

void TunerPanel::timerCallback()
{
    updateInputs();
    PitchResult r;
    {
        const SpinLock::ScopedLockType sl(resultLock);
        if (!hasNew) return;
        r = latest;
        hasNew = false;
    }
    display.setResult(r, reference, flatsCircle.getToggleState());
}

void TunerPanel::paint(Graphics & g)
{
    g.fillAll(panelBackground);
}

void TunerPanel::resized()
{
    auto r = getLocalBounds().reduced(8);
    const int d = IconCircle::size;
    auto row = ToolRail::row(*this, r, 8);
    row = row.withSizeKeepingCentre(row.getWidth(), d + 4);
    inputPicker.setBounds(row.getX(), row.getY(), inputPicker.getOpenWidth(), d + 4);
    row.removeFromLeft(inputPicker.getClosedWidth() + 14);
    auto place = [&](Component & c, int w) { c.setBounds(row.removeFromLeft(w).withSizeKeepingCentre(w, d)); row.removeFromLeft(4); };
    place(refDown, d);
    place(refLabel, 64);
    place(refUp, d);
    row.removeFromLeft(10);
    place(flatsCircle, d);
    if (inputPicker.isOpen()) inputPicker.toFront(false);

    display.setBounds(r.reduced(0, 4));
}

}
