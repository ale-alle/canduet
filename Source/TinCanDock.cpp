// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanDock.h"
#include "TinCanTheme.h"

namespace TinCan {

DockHeaderButton::DockHeaderButton() : Button({}) { setMouseCursor(MouseCursor::PointingHandCursor); }

void DockHeaderButton::set(const String & text, Glyph g, const String & tip)
{
    setButtonText(text);
    glyph = g;
    setTooltip(tip);
    setTitle(tip);
    repaint();
}

int DockHeaderButton::idealWidth() const
{
    const auto t = getButtonText();
    if (t.isEmpty()) return 28;
    return 14 + (glyph != Glyph::none ? 18 : 0)
           + Font(13.0f, Font::bold).getStringWidth(t);
}

void DockHeaderButton::paintButton(Graphics & g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f, 3.5f);
    g.setColour(down ? TinCanTheme::lineStrong : over ? TinCanTheme::raised.brighter(0.15f) : TinCanTheme::raised);
    g.fillRoundedRectangle(r, 7.0f);
    g.setColour(over ? TinCanTheme::string : TinCanTheme::line);
    g.drawRoundedRectangle(r, 7.0f, 1.0f);

    g.setColour(over ? TinCanTheme::text : TinCanTheme::muted);
    auto inner = r.reduced(7.0f, 0.0f);
    const bool hasText = getButtonText().isNotEmpty();
    Path p;
    if (glyph == Glyph::menu) {
        // a small down arrow at the right, after the text
        auto a = inner.removeFromRight(10.0f).withSizeKeepingCentre(10.0f, 6.0f);
        inner.removeFromRight(6.0f);
        p.startNewSubPath(a.getX(), a.getY()); p.lineTo(a.getCentreX(), a.getBottom()); p.lineTo(a.getRight(), a.getY());
    }
    else if (glyph != Glyph::none) {
        auto box = hasText ? inner.removeFromLeft(12.0f).withSizeKeepingCentre(12.0f, 12.0f)
                           : inner.withSizeKeepingCentre(12.0f, 12.0f);
        if (hasText) inner.removeFromLeft(6.0f);
        const float x = box.getX(), y = box.getY(), w = box.getWidth();
        if (glyph == Glyph::close) {
            p.startNewSubPath(x + 1, y + 1); p.lineTo(x + w - 1, y + w - 1);
            p.startNewSubPath(x + w - 1, y + 1); p.lineTo(x + 1, y + w - 1);
        }
        else {
            // a box with an arrow leaving it (pop out) or entering it (dock in)
            p.startNewSubPath(x + w * 0.45f, y + 1); p.lineTo(x + 1, y + 1); p.lineTo(x + 1, y + w - 1);
            p.lineTo(x + w - 1, y + w - 1); p.lineTo(x + w - 1, y + w * 0.55f);
            if (glyph == Glyph::popOut) {
                p.startNewSubPath(x + w * 0.45f, y + w * 0.55f); p.lineTo(x + w - 1, y + 1);
                p.startNewSubPath(x + w * 0.62f, y + 1); p.lineTo(x + w - 1, y + 1); p.lineTo(x + w - 1, y + w * 0.38f);
            }
            else {
                p.startNewSubPath(x + w - 1, y + 1); p.lineTo(x + w * 0.45f, y + w * 0.55f);
                p.startNewSubPath(x + w * 0.45f, y + w * 0.2f); p.lineTo(x + w * 0.45f, y + w * 0.55f); p.lineTo(x + w * 0.8f, y + w * 0.55f);
            }
        }
    }
    if (!p.isEmpty()) g.strokePath(p, PathStrokeType(1.7f, PathStrokeType::curved, PathStrokeType::rounded));
    if (hasText) {
        g.setFont(Font(13.0f, Font::bold));
        g.drawText(getButtonText(), inner, Justification::centredLeft, true);
    }
}

DockHolder::DockHolder(const String & t) : title(t)
{
    setOpaque(true);
    dockButton = std::make_unique<DockHeaderButton>();
    closeButton = std::make_unique<DockHeaderButton>();
    closeButton->set({}, DockHeaderButton::Glyph::close, TRANS("Close") + " " + title);
    dockButton->onClick = [this]() { if (onToggleDock) onToggleDock(); };
    closeButton->onClick = [this]() { if (onClose) onClose(); };
    addChildComponent(dockButton.get());
    addChildComponent(closeButton.get());
    setDocked(false);
}

DockHolder::~DockHolder() = default;

void DockHolder::setBody(Component * newBody)
{
    if (body != nullptr) removeChildComponent(body);
    body = newBody;
    if (body != nullptr) addAndMakeVisible(body);
    resized();
}

void DockHolder::setDocked(bool d)
{
    docked = d;
    if (docked) dockButton->set(TRANS("Pop out"), DockHeaderButton::Glyph::popOut, TRANS("Move") + " " + title + " " + TRANS("to a window of its own"));
    else dockButton->set(TRANS("Dock in lesson"), DockHeaderButton::Glyph::dockIn, TRANS("Put") + " " + title + " " + TRANS("back inside the lesson window"));
    dockButton->setVisible(docked || canDock);
    closeButton->setVisible(docked);   // its own window has the title bar's close
    resized();
    repaint();
}

void DockHolder::addHeaderItem(Component * item)
{
    headerItems.add(item);
    addAndMakeVisible(item);
    resized();
}

void DockHolder::setCanDock(bool can)
{
    canDock = can;
    setDocked(docked);
}

void DockHolder::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);
    auto head = getLocalBounds().removeFromTop(headerHeight);
    g.setColour(TinCanTheme::line);
    g.fillRect(head.removeFromBottom(1));
    g.setColour(TinCanTheme::text);
    g.setFont(Font(14.0f, Font::bold));
    g.drawText(title, head.withTrimmedLeft(12).withTrimmedRight(160), Justification::centredLeft, true);
    if (docked) {
        g.setColour(TinCanTheme::line);
        g.drawRect(getLocalBounds(), 1);
    }
}

void DockHolder::resized()
{
    auto r = getLocalBounds();
    auto head = r.removeFromTop(headerHeight).reduced(6, 0);
    if (closeButton->isVisible()) {
        closeButton->setBounds(head.removeFromRight(30));
        head.removeFromRight(6);
    }
    if (dockButton->isVisible()) {
        dockButton->setBounds(head.removeFromRight(dockButton->idealWidth()));
        head.removeFromRight(6);
    }
    for (auto * item : headerItems) {
        const int w = item->getWidth() > 0 ? item->getWidth() : 160;
        item->setBounds(head.removeFromRight(jmin(w, jmax(0, head.getWidth() - 120))));
        head.removeFromRight(6);
    }
    if (body != nullptr) body->setBounds(docked ? r.reduced(1, 0).withTrimmedBottom(1) : r);
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
