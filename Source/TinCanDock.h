// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "TinCanIcons.h"

#include <functional>
#include <memory>

// Dockable tools (T-085): Tools, Board and Score each live in a window of their own, or docked inside
// the lesson window. The tool itself sits in a DockHolder that moves between its own DockableWindow
// and the lesson window's dock frame. Moving a web page between windows needs the WebView2 to follow
// its component (patched in deps/juce, juce_WebBrowserComponent_windows.cpp).
//
// Redesign (T-089): no title row. Two round icons, pop out / dock and close, sit at the right end of
// the tool's own bar: drawn here in the top-right corner over a tool made of components, or drawn by
// a web page itself (nothing native can be drawn over a WebView2), which asks for them by message.
namespace TinCan {

class DockHolder : public Component
{
public:
    explicit DockHolder(const String & title);
    ~DockHolder() override;

    void setBody(Component * body);   // not owned
    Component * getBody() const { return body; }

    // docked: pop out and close; in its own window: dock in (when there is a lesson)
    void setDocked(bool docked);
    void setCanDock(bool can);
    bool isDocked() const { return docked; }
    bool canDockIn() const { return canDock; }

    // a tool that draws the two icons itself (a web page): told the state, and calls the requests
    void setControlsInBody(std::function<void(bool docked, bool canDock)> onStateChanged);
    void requestToggleDock() { if (onToggleDock) onToggleDock(); }
    void requestClose() { if (onClose) onClose(); }

    std::function<void()> onToggleDock, onClose;

    // the room the corner icons take at the top right of a tool's bar
    static constexpr int cornerIcon = 30, cornerWidth = 2 * cornerIcon + 14;
    // a tool made of components keeps its icon bar this tall, so the corner icons sit centred in it
    static constexpr int railHeight = 52;

    void paint(Graphics & g) override;
    void resized() override;

private:
    void stateChanged();

    String title;
    Component * body = nullptr;
    bool docked = false, canDock = false;
    IconCircle dockButton, closeButton;
    std::function<void(bool, bool)> bodyControls;

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
