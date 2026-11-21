// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "TinCanPitch.h"
#include "TinCanIcons.h"

class SonobusAudioProcessor;

// Tuner and metronome tabs of the teaching tools window (T-036).
// The metronome is SonoBus's own (same on/off, tempo, volume and "send" settings as the toolbar
// metronome), with beats per bar, subdivisions, count-in and a beat indicator added.
// The tuner listens to one local input on its own thread while its tab is showing; it makes no
// sound and nothing it hears is sent anywhere.
namespace TinCan {

class BeatIndicator : public Component
{
public:
    void update(double beatPosition, int beatsPerBar, int subdivision, double countInRemaining, bool running);
    void paint(Graphics & g) override;

private:
    double position = 0.0;
    int beatsPerBar = 1;
    int subdivision = 1;
    bool countIn = false;
    bool running = false;
};

class MetronomePanel : public Component, private Timer
{
public:
    explicit MetronomePanel(SonobusAudioProcessor & processor);
    ~MetronomePanel() override;

    void resized() override;
    void paint(Graphics & g) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void setIntParam(const String & id, int value);
    int getIntParam(const String & id) const;
    void tapTempo();

    void setTempo(int bpm);

    SonobusAudioProcessor & processor;

    // one bar of round icons (T-089): start, tempo, tap, beats, clicks per beat, count-in, volume, send
    IconCircle playCircle, slowerCircle, fasterCircle, tapCircle, volumeIcon, sendCircle;
    Slider tempoSlider { Slider::LinearBar, Slider::TextBoxLeft };   // a bar with the number on it
    IconPicker beatsPicker, subdivisionPicker, countInPicker;
    Slider volumeSlider { Slider::LinearHorizontal, Slider::NoTextBox };
    BeatIndicator indicator;

    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> enableAttachment, sendAttachment;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> tempoAttachment, volumeAttachment;

    Array<double> taps;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetronomePanel)
};

class TunerDisplay : public Component, public SettableTooltipClient
{
public:
    void setResult(const PitchResult & result, double a4, bool useFlats);
    void paint(Graphics & g) override;

private:
    PitchResult result;
    double note = 0.0;      // fractional MIDI note
    double shownCents = 0.0; // smoothed needle
    bool useFlats = false;
};

class TunerPanel : public Component, private Timer, private Thread
{
public:
    explicit TunerPanel(SonobusAudioProcessor & processor);
    ~TunerPanel() override;

    void resized() override;
    void paint(Graphics & g) override;
    void visibilityChanged() override;
    void mouseDoubleClick(const MouseEvent & e) override;   // on the reference A: back to 440

private:
    void timerCallback() override;
    void run() override;  // analysis thread
    void startListening();
    void stopListening();
    void updateInputs();

    void setReference(double hz);
    int getInput() const { return jmax(1, inputPicker.getSelected()); }

    SonobusAudioProcessor & processor;

    // one bar of round icons (T-089): which input, the reference A, sharps or flats
    IconPicker inputPicker;
    IconCircle refDown, refUp, flatsCircle;
    Label refLabel;
    double reference = 440.0;
    TunerDisplay display;

    // result handed from the analysis thread to the UI
    SpinLock resultLock;
    PitchResult latest;
    bool hasNew = false;
    int numInputs = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TunerPanel)
};

}
