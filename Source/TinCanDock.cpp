// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanDock.h"
#include "TinCanTheme.h"

namespace TinCan {

DockHolder::DockHolder(const String & t) : title(t)
{
    setOpaque(true);
    closeButton.setIcon(Icons::close());
    closeButton.setTooltip(TRANS("Close") + " " + title);
    closeButton.setTitle(TRANS("Close") + " " + title);
    dockButton.onClick = [this]() { requestToggleDock(); };
    closeButton.onClick = [this]() { requestClose(); };
    addChildComponent(dockButton);
    addChildComponent(closeButton);
    setDocked(false);
}

DockHolder::~DockHolder() = default;

void DockHolder::setBody(Component * newBody)
{
    if (body != nullptr) removeChildComponent(body);
    body = newBody;
    if (body != nullptr) addAndMakeVisible(body);
    // the corner icons stay over the tool
    dockButton.toFront(false);
    closeButton.toFront(false);
    resized();
}

void DockHolder::setControlsInBody(std::function<void(bool, bool)> onStateChanged)
{
    bodyControls = std::move(onStateChanged);
    stateChanged();
}

void DockHolder::setDocked(bool d)
{
    docked = d;
    stateChanged();
}

void DockHolder::setCanDock(bool can)
{
    canDock = can;
    stateChanged();
}

void DockHolder::stateChanged()
{
    const auto tip = docked ? TRANS("Pop out") + " " + title + ": " + TRANS("a window of its own")
                            : TRANS("Dock") + " " + title + " " + TRANS("in the lesson window");
    dockButton.setIcon(docked ? Icons::popOut() : Icons::dockIn());
    dockButton.setTooltip(tip);
    dockButton.setTitle(tip);
    // in its own window the title bar has the close button
    const bool drawn = !bodyControls;
    dockButton.setVisible(drawn && (docked || canDock));
    closeButton.setVisible(drawn && docked);
    if (bodyControls) bodyControls(docked, canDock);
    resized();
    repaint();
}

void DockHolder::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);
    if (docked) {
        g.setColour(TinCanTheme::line);
        g.drawRect(getLocalBounds(), 1);
    }
}

void DockHolder::resized()
{
    auto r = getLocalBounds();
    if (body != nullptr) body->setBounds(docked ? r.reduced(1) : r);
    // the corner: at the right end of the tool's own bar
    auto corner = r.reduced(docked ? 1 : 0).removeFromTop(railHeight).reduced(6, (railHeight - cornerIcon) / 2);
    if (closeButton.isVisible()) {
        closeButton.setBounds(corner.removeFromRight(cornerIcon));
        corner.removeFromRight(4);
    }
    if (dockButton.isVisible()) dockButton.setBounds(corner.removeFromRight(cornerIcon));
}

DockableWindow::DockableWindow(const String & title, int w, int h)
    : DocumentWindow(title, TinCanTheme::night, DocumentWindow::allButtons), defaultW(w), defaultH(h)
{
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    setResizeLimits(480, 240, 10000, 10000);
    holder = std::make_unique<DockHolder>(title);
    holder->onToggleDock = [this]() { if (onToggleDock) onToggleDock(); };
    holder->onClose = [this]() { if (onClose) onClose(); else closeButtonPressed(); };
    setContentNonOwned(holder.get(), false);
    centreWithSize(w, h);
}

DockableWindow::~DockableWindow()
{
    clearContentComponent();
    holder.reset();   // takes it out of the dock if it's there
    body.reset();
}

void DockableWindow::setBody(std::unique_ptr<Component> b)
{
    holder->setBody(b.get());
    body = std::move(b);
}

void DockableWindow::dockInto(Component & frame)
{
    if (docked && holder->getParentComponent() == &frame) return;
    if (isVisible()) {
        if (!isMinimised()) placed = true;
        setVisible(false);
    }
    clearContentComponent();
    holder->setDocked(true);
    frame.addAndMakeVisible(holder.get());
    holder->setBounds(frame.getLocalBounds());
    docked = true;
}

void DockableWindow::takeBack()
{
    if (!docked) return;
    docked = false;
    holder->setDocked(false);
    setContentNonOwned(holder.get(), false);
}

void DockableWindow::showFloating()
{
    takeBack();
    if (!placed) {
        centreWithSize(defaultW, defaultH);
        placed = true;
    }
    setVisible(true);
    if (isMinimised()) setMinimised(false);
    toFront(true);
}

void DockableWindow::restoreState(const String & state)
{
    if (state.isNotEmpty() && restoreWindowStateFromString(state)) placed = true;
}

void DockableWindow::closeButtonPressed()
{
    if (onClose) onClose();
    else setVisible(false);
}

}
