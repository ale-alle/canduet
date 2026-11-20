// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

#include <functional>
#include <memory>

// Dockable tools (T-085): Tools, Board and Score each live in a window of their own, or docked inside
// the lesson window. The tool itself sits in a DockHolder (a thin header over it) that moves between
// its own DockableWindow and the lesson window's dock frame. Moving a web page between windows needs
// the WebView2 to follow its component (patched in deps/juce, juce_WebBrowserComponent_windows.cpp).
namespace TinCan {

// a small pill for a tool's header: text with an optional line icon in front
class DockHeaderButton : public Button
{
public:
    enum class Glyph { none, popOut, dockIn, close, menu };

    DockHeaderButton();
    void set(const String & text, Glyph glyph, const String & tip);
    int idealWidth() const;
    void paintButton(Graphics & g, bool over, bool down) override;

private:
    Glyph glyph = Glyph::none;
};

class DockHolder : public Component
{
public:
    explicit DockHolder(const String & title);
    ~DockHolder() override;

    void setBody(Component * body);   // not owned
    Component * getBody() const { return body; }

    // docked: "Pop out" and close; in its own window: "Dock in lesson" (when there is a lesson)
    void setDocked(bool docked);
    void setCanDock(bool can);

    // a tool's own control in the header, left of Dock / Pop out (not owned); call relayout() after
    // its size changes
    void addHeaderItem(Component * item);
    void relayout() { resized(); }

    std::function<void()> onToggleDock, onClose;

    static constexpr int headerHeight = 32;

    void paint(Graphics & g) override;
    void resized() override;

private:
    String title;
    Component * body = nullptr;
    bool docked = false, canDock = false;
    std::unique_ptr<DockHeaderButton> dockButton, closeButton;
    Array<Component *> headerItems;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DockHolder)
};

// A tool's own window. Closing it only hides it; the lesson window moves its holder in and out.
class DockableWindow : public DocumentWindow
{
public:
    DockableWindow(const String & title, int defaultWidth, int defaultHeight);
    ~DockableWindow() override;

    void setBody(std::unique_ptr<Component> body);
    Component * getBody() const { return body.get(); }
    DockHolder & getHolder() { return *holder; }

    // the lesson window's dock takes the holder (this window hides) and gives it back (stays hidden)
    void dockInto(Component & frame);
    void takeBack();
    bool isDocked() const { return docked; }

    // shows it on its own, where it was last time (restoreState) or centred
    void showFloating();
    String saveState() { return getWindowStateAsString(); }
    void restoreState(const String & state);

    // the header's Dock / Pop out button, and closing (title bar or the docked header's close)
    std::function<void()> onToggleDock, onClose;

    void closeButtonPressed() override;

private:
    std::unique_ptr<Component> body;
    std::unique_ptr<DockHolder> holder;
    bool docked = false;
    bool placed = false;   // given a position yet
    int defaultW, defaultH;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DockableWindow)
};

}
