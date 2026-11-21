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
    int getClosedWidth() const { return IconCircle::size + 2 * pad; }
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
    bool open = false;
};

}
