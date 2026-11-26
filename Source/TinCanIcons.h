// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

#include <functional>
#include <vector>

// Round icon buttons in the style of tincanstudios.ca/lessons (a soft red circle with a red line
// icon; bright red with a white icon when chosen or on), and a picker made of them that opens
// to the right on hover and folds back to show only the choice. Icons are drawn from SVG path
// data (Lucide-style, 24x24, 2 px lines) so they stay sharp at any size.
namespace TinCan {

namespace Icons {
    // line icons (stroked) and solid parts (filled), in a 24x24 box
    struct Icon
    {
        Path line;
        Path solid;
    };
    Icon piano();
    Icon guitar();
    Icon chevronLeft();
    Icon chevronRight();
    Icon cast();      // sharing my keys
    Icon eye();       // showing the other person's keys
    Icon midi();      // a 5-pin MIDI socket
    Icon popOut();    // a box with an arrow leaving it: into a window of its own
    Icon dockIn();    // a box with an arrow going down into it: back inside the lesson window
    Icon close();     // an X
    // Tools (T-089)
    Icon tuner();     // a gauge with a needle
    Icon metronome();
    Icon notes();     // two beamed notes: the instrument
    Icon summaries(); // a list
    Icon homework();  // a clipboard with ticks (T-090)
    Icon recording(); // a ring with a dot (T-090)
    Icon board();
    Icon play();
    Icon stop();
    Icon minus();
    Icon plus();
    Icon tap();       // a finger tapping: ripples round a dot
    Icon quarter();   // one click per beat
    Icon eighths();   // two
    Icon triplet();   // three
    Icon sixteenths();// four
    Icon flag();      // a highlight
    Icon folder();
    Icon trash();
    Icon volume();
    Icon mic();
}

// The Tools panels' icon bar (T-089): one row this tall across the top of every panel. The Tools
// picker sits at its left and the dock icons at its right; a panel lays its own icons between,
// leaving the room the Tools panel gives it (properties "railLeft" and "railRight").
namespace ToolRail {
    constexpr int height = 52;
    inline int left(const Component & c) { return (int) c.getProperties().getWithDefault("railLeft", 0); }
    inline int right(const Component & c) { return (int) c.getProperties().getWithDefault("railRight", 0); }
    // the row between them, inside a panel's area r (already reduced by its margin)
    // (r starts margin px in from the panel's top; the row is the full bar height from the top)
    inline juce::Rectangle<int> row(const Component & c, juce::Rectangle<int> & r, int margin)
    {
        juce::Rectangle<int> row(r.getX(), r.getY() - margin, r.getWidth(), height);
        r.removeFromTop(height - margin);
        row.removeFromLeft(jmax(0, left(c) - margin));
        row.removeFromRight(jmax(0, right(c) - margin));
        return row;
    }
}

class IconCircle : public Button
{
public:
    IconCircle() : Button({}) {}

    void setIcon(Icons::Icon newIcon) { icon = std::move(newIcon); repaint(); }
    // text instead of (or with) an icon, e.g. "61", "C4", "♭"; badge is a small number at the lower right
    void setText(const String & newText) { text = newText; repaint(); }
    void setBadge(const String & newBadge) { badge = newBadge; repaint(); }
    // "on" shows bright red; the toggle state is used for it
    void setOn(bool on) { setToggleState(on, dontSendNotification); }

    void paintButton(Graphics & g, bool highlighted, bool down) override;

    static constexpr int size = 40;

private:
    Icons::Icon icon;
    String text, badge;
};

// One circle shows the choice; hovering (or clicking it) opens the other choices to the right;
// picking one folds them back to the left, leaving only the choice, bright red.
class IconPicker : public Component, private Timer
{
public:
    struct Option
    {
        int id = 0;
        Icons::Icon icon;
        String text, badge, tip;
    };

    IconPicker();
    ~IconPicker() override;

    void setOptions(std::vector<Option> options, int selectedId);
    void setSelected(int id);   // no onChange
    int getSelected() const { return selected; }
    std::function<void(int id)> onChange;
    // opened or closed (the panel hides what the open choices cover)
    std::function<void(bool open)> onOpenChanged;

    // the room it takes when closed (it draws over its neighbours when open)
    int getClosedWidth() const { return alwaysOpen ? getOpenWidth() : IconCircle::size + 2 * pad; }

    // owner 2026-11-25: every choice shown side by side, the current one bright red; nothing slides
    void setAlwaysOpen(bool shouldBe);
    int getOpenWidth() const;

    bool hitTest(int x, int y) override;
    void paint(Graphics & g) override;
    void resized() override { layout(false); }
    void mouseEnter(const MouseEvent &) override;
    void mouseExit(const MouseEvent &) override;

    bool isOpen() const { return open; }
    void setOpen(bool shouldBeOpen);

private:
    void timerCallback() override;
    void layout(bool animate);
    void pick(int id);
    juce::Rectangle<int> slot(int index) const;

    static constexpr int gap = 6, pad = 2;
    std::vector<Option> options;
    OwnedArray<IconCircle> circles;   // same order as options
    int selected = 0;
    bool pendingOpen = false;   // hovering: opens when the second is up
    bool alwaysOpen = false;
    bool open = false;
};

}
