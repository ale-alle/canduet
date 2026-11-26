// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanIcons.h"
#include "TinCanTheme.h"

namespace TinCan {

namespace Icons {

static Path svg(const char * d) { return Drawable::parseSVGPath(d); }

Icon piano()
{
    // a keyboard: the outline and white-key lines; two black keys solid
    Icon i;
    i.line.addRoundedRectangle(3.0f, 5.0f, 18.0f, 14.0f, 2.0f);
    i.line.startNewSubPath(9.0f, 12.0f); i.line.lineTo(9.0f, 19.0f);
    i.line.startNewSubPath(15.0f, 12.0f); i.line.lineTo(15.0f, 19.0f);
    i.solid.addRoundedRectangle(7.2f, 5.0f, 3.6f, 7.5f, 0.6f);
    i.solid.addRoundedRectangle(13.2f, 5.0f, 3.6f, 7.5f, 0.6f);
    return i;
}

Icon guitar()
{
    // Lucide "guitar" (ISC licence)
    Icon i;
    i.line = svg("m11.9 12.1 4.514-4.514");
    i.line.addPath(svg("M20.1 2.3a1 1 0 0 0-1.4 0l-1.114 1.114A2 2 0 0 0 17 4.828v1.344a2 2 0 0 1-.586 1.414A2 2 0 0 1 17.828 7h1.344a2 2 0 0 0 1.414-.586L21.7 5.3a1 1 0 0 0 0-1.4z"));
    i.line.addPath(svg("m6 16 2 2"));
    i.line.addPath(svg("M8.2 9.9C8.7 8.8 9.8 8 11 8c2.8 0 5 2.2 5 5 0 1.2-.8 2.3-1.9 2.8l-.9.4A2 2 0 0 0 12 18a4 4 0 0 1-4 4c-3.3 0-6-2.7-6-6a4 4 0 0 1 4-4 2 2 0 0 0 1.8-1.2z"));
    i.solid.addEllipse(10.6f, 11.6f, 1.8f, 1.8f);
    return i;
}

Icon chevronLeft() { Icon i; i.line = svg("m15 18-6-6 6-6"); return i; }
Icon chevronRight() { Icon i; i.line = svg("m9 18 6-6-6-6"); return i; }

Icon cast()
{
    // Lucide "cast"
    Icon i;
    i.line = svg("M2 8V6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-6");
    i.line.addPath(svg("M2 12a9 9 0 0 1 8 8"));
    i.line.addPath(svg("M2 16a5 5 0 0 1 4 4"));
    i.solid.addEllipse(1.2f, 19.2f, 1.6f, 1.6f);
    return i;
}

Icon eye()
{
    // Lucide "eye"
    Icon i;
    i.line = svg("M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0");
    i.line.addEllipse(9.0f, 9.0f, 6.0f, 6.0f);
    return i;
}

// the dock icons, the same drawings as the pages' (redesign T-089)
Icon popOut()
{
    Icon i;
    i.line = svg("M14 4h6v6M20 4l-8 8M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5");
    return i;
}

Icon dockIn()
{
    Icon i;
    i.line.addRoundedRectangle(3.0f, 4.0f, 18.0f, 16.0f, 2.0f);
    i.line.addPath(svg("M3 15h18M12 6.5v5.5M9 9.5l3 3 3-3"));
    return i;
}

Icon close()
{
    Icon i;
    i.line = svg("M6.5 6.5l11 11M17.5 6.5l-11 11");
    return i;
}

static Icon lineIcon(const char * d) { Icon i; i.line = svg(d); return i; }

Icon tuner()
{
    Icon i;
    i.line = svg("M4 16a8 8 0 0 1 16 0M12 16l3.5-5.5");
    i.solid.addEllipse(10.6f, 14.6f, 2.8f, 2.8f);
    return i;
}
Icon metronome() { return lineIcon("M8.5 3h7l4 18h-15zM12 17l4.5-9.5M6.6 14h10.8"); }
Icon notes()
{
    Icon i;
    i.line = svg("M9 18V5l12-2v13");
    i.solid.addEllipse(3.2f, 15.2f, 5.8f, 5.6f);
    i.solid.addEllipse(15.2f, 13.2f, 5.8f, 5.6f);
    return i;
}
Icon summaries() { return lineIcon("M8 6h13M8 12h13M8 18h13M3.5 6h.5M3.5 12h.5M3.5 18h.5"); }
Icon homework() { return lineIcon("M9 4h6v3H9zM9 5.5H6a1 1 0 0 0-1 1V20a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V6.5a1 1 0 0 0-1-1h-3M8 12l1.5 1.5L12 11M8 17l1.5 1.5L12 16M14 12.5h3M14 17.5h3"); }
Icon recording() { Icon i; i.line.addEllipse(3.0f, 3.0f, 18.0f, 18.0f); i.solid.addEllipse(8.5f, 8.5f, 7.0f, 7.0f); return i; }
Icon board() { return lineIcon("M3 4h18v13H3zM8 21l4-4 4 4M7 13l3-3 2 2 4-4"); }
Icon play() { Icon i; i.solid = svg("M7 4.5 19.5 12 7 19.5z"); return i; }
Icon stop() { Icon i; i.solid.addRoundedRectangle(6.5f, 6.5f, 11.0f, 11.0f, 1.5f); return i; }
Icon minus() { return lineIcon("M5 12h14"); }
Icon plus() { return lineIcon("M12 5v14M5 12h14"); }
Icon tap()
{
    Icon i;
    i.line = svg("M6.3 8.5a6.5 6.5 0 0 1 11.4 0M3.6 6a9.8 9.8 0 0 1 16.8 0M12 11v10");
    i.solid.addEllipse(9.5f, 8.5f, 5.0f, 5.0f);
    return i;
}
static Icon beamed(int n)
{
    // n notes on one or two beams (two for sixteenths), small and filled
    Icon i;
    const float x0 = n == 1 ? 9.0f : 4.0f, step = n == 1 ? 0.0f : 16.0f / (float) (n - 1);
    for (int k = 0; k < n; ++k) {
        const float x = x0 + k * step;
        i.solid.addEllipse(x - 2.6f, 16.2f, 5.0f, 3.8f);
        i.line.startNewSubPath(x + 2.2f, 18.0f); i.line.lineTo(x + 2.2f, 5.0f);
    }
    if (n > 1) {
        i.solid.addRectangle(x0 + 1.4f, 4.0f, step * (n - 1) + 1.6f, 2.2f);
        if (n == 4) i.solid.addRectangle(x0 + 1.4f, 7.6f, step * (n - 1) + 1.6f, 2.0f);
    }
    return i;
}
Icon quarter() { return beamed(1); }
Icon eighths() { return beamed(2); }
Icon triplet() { return beamed(3); }
Icon sixteenths() { return beamed(4); }
Icon flag() { return lineIcon("M5 21V4M5 4h12l-2.5 4 2.5 4H5"); }
Icon folder() { return lineIcon("M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"); }
Icon trash() { return lineIcon("M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"); }
Icon volume() { return lineIcon("M4 9h4l5-4v14l-5-4H4zM16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"); }
Icon mic() { return lineIcon("M9 5.5a3 3 0 0 1 6 0v6a3 3 0 0 1-6 0zM5 10.5a7 7 0 0 0 14 0M12 17.5V21M8.5 21h7"); }

Icon midi()
{
    // a 5-pin DIN socket, as on MIDI cables
    Icon i;
    i.line.addEllipse(3.0f, 3.0f, 18.0f, 18.0f);
    i.line.startNewSubPath(10.5f, 3.4f); i.line.lineTo(13.5f, 3.4f);
    const float pins[5][2] = { { 7.0f, 12.0f }, { 17.0f, 12.0f }, { 8.5f, 8.5f }, { 15.5f, 8.5f }, { 12.0f, 7.0f } };
    for (auto & p : pins) i.solid.addEllipse(p[0] - 1.2f, p[1] - 1.2f, 2.4f, 2.4f);
    return i;
}

}

void IconCircle::paintButton(Graphics & g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    r = r.withSizeKeepingCentre(jmin(r.getWidth(), r.getHeight()), jmin(r.getWidth(), r.getHeight()));
    const bool on = getToggleState();
    const float alpha = isEnabled() ? 1.0f : 0.4f;

    // the circle: soft red (the site's string-soft on night), bright red when on
    Colour fill = on ? TinCanTheme::stringStrong : TinCanTheme::string.withAlpha(highlighted ? 0.30f : 0.16f);
    if (down) fill = fill.brighter(0.15f);
    g.setColour(fill.withMultipliedAlpha(alpha));
    g.fillEllipse(r);
    g.setColour((on ? TinCanTheme::stringStrong.brighter(0.25f) : TinCanTheme::string.withAlpha(highlighted ? 0.75f : 0.45f)).withMultipliedAlpha(alpha));
    g.drawEllipse(r.reduced(0.6f), 1.2f);

    const Colour ink = (on ? Colours::white : TinCanTheme::stringLight).withMultipliedAlpha(alpha);
    g.setColour(ink);
    if (!icon.line.isEmpty() || !icon.solid.isEmpty()) {
        // the 24x24 icon in the middle half of the circle
        auto box = r.reduced(r.getWidth() * (text.isNotEmpty() ? 0.30f : 0.24f));
        auto t = AffineTransform::scale(box.getWidth() / 24.0f).translated(box.getX(), box.getY());
        g.strokePath(icon.line, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded), t);
        g.fillPath(icon.solid, t);
    }
    else if (text.isNotEmpty()) {
        // a lone music sign (♭) is small in the font: larger, so it reads like the letters
        float fs = text.length() == 1 && text[0] > 0x2000 ? 24.0f : text.length() <= 2 ? 15.0f : text.length() <= 3 ? 13.0f : 11.0f;
        g.setFont(Font(fs, Font::bold));
        g.drawText(text, r, Justification::centred, false);
    }
    if (badge.isNotEmpty()) {
        // a small number at the lower right, in its own dot
        auto b = juce::Rectangle<float>(15.0f, 15.0f).withCentre({ r.getRight() - 6.0f, r.getBottom() - 6.0f });
        g.setColour((on ? Colours::white : TinCanTheme::stringStrong).withMultipliedAlpha(alpha));
        g.fillEllipse(b);
        g.setColour((on ? TinCanTheme::stringStrong : Colours::white).withMultipliedAlpha(alpha));
        g.setFont(Font(11.0f, Font::bold));
        g.drawText(badge, b, Justification::centred, false);
    }
}

//==============================================================================

IconPicker::IconPicker()
{
    setInterceptsMouseClicks(false, true);
}

IconPicker::~IconPicker()
{
    stopTimer();
    for (auto * c : circles) Desktop::getInstance().getAnimator().cancelAnimation(c, false);
}

void IconPicker::setOptions(std::vector<Option> newOptions, int selectedId)
{
    options = std::move(newOptions);
    circles.clear();
    for (auto & o : options) {
        auto * c = circles.add(new IconCircle());
        c->setIcon(o.icon);
        c->setText(o.text);
        c->setBadge(o.badge);
        c->setTooltip(o.tip);
        c->setTitle(o.tip);
        c->addMouseListener(this, false);
        const int id = o.id;
        c->onClick = [this, id]() { pick(id); };
        addChildComponent(c);
    }
    selected = selectedId;
    open = alwaysOpen;
    layout(false);
}

void IconPicker::setAlwaysOpen(bool shouldBe)
{
    alwaysOpen = shouldBe;
    open = shouldBe;
    layout(false);
}

void IconPicker::setSelected(int id)
{
    selected = id;
    layout(false);
}

int IconPicker::getOpenWidth() const
{
    const int n = (int) options.size();
    return n * IconCircle::size + jmax(0, n - 1) * gap + 2 * pad;
}

juce::Rectangle<int> IconPicker::slot(int index) const
{
    return { pad + index * (IconCircle::size + gap), (getHeight() - IconCircle::size) / 2, IconCircle::size, IconCircle::size };
}

bool IconPicker::hitTest(int x, int y)
{
    // closed: only the one circle answers, so the space it may open into stays usable
    if (open) return true;
    return slot(0).expanded(pad).contains(x, y);
}

void IconPicker::paint(Graphics & g)
{
    if (!open || alwaysOpen) return;
    // a pill behind the open choices, so they read over whatever is under them
    auto r = juce::Rectangle<int>(0, slot(0).getY() - pad, getOpenWidth(), IconCircle::size + 2 * pad).toFloat();
    g.setColour(TinCanTheme::raised.withAlpha(0.96f));
    g.fillRoundedRectangle(r, r.getHeight() / 2);
    g.setColour(TinCanTheme::line);
    g.drawRoundedRectangle(r.reduced(0.5f), r.getHeight() / 2, 1.0f);
}

void IconPicker::layout(bool animate)
{
    auto & animator = Desktop::getInstance().getAnimator();
    const int ms = 160;
    // the choice first, then the others in their own order
    int k = 1;
    for (size_t i = 0; i < options.size(); ++i) {
        auto * c = circles[(int) i];
        const bool isChoice = options[i].id == selected;
        c->setOn(isChoice);
        auto target = alwaysOpen ? slot((int) i) : isChoice ? slot(0) : slot(k++);
        if (isChoice || open) {
            if (!c->isVisible()) { c->setBounds(slot(0)); c->setAlpha(isChoice ? 1.0f : 0.0f); c->setVisible(true); }
            if (animate) animator.animateComponent(c, target, 1.0f, ms, false, 0.4, 0.6);
            else { animator.cancelAnimation(c, false); c->setBounds(target); c->setAlpha(1.0f); }
        }
        else if (c->isVisible()) {
            // fold back into the choice and disappear
            if (animate) {
                animator.animateComponent(c, slot(0), 0.0f, ms, false, 0.4, 0.6);
                Component::SafePointer<IconCircle> safe(c);
                Timer::callAfterDelay(ms + 30, [safe, this]() { if (safe && !open) safe->setVisible(false); });
            }
            else { animator.cancelAnimation(c, false); c->setVisible(false); }
        }
        if (isChoice) c->toFront(false);
    }
    repaint();
}

void IconPicker::setOpen(bool shouldBeOpen)
{
    pendingOpen = false;
    if (alwaysOpen || open == shouldBeOpen) return;
    open = shouldBeOpen;
    if (open) toFront(false);
    layout(true);
    if (onOpenChanged) onOpenChanged(open);
}

void IconPicker::pick(int id)
{
    if (!open) { setOpen(true); return; }   // a click on the closed circle opens it (touch, keyboard)
    const bool changed = id != selected;
    selected = id;
    if (alwaysOpen) {
        layout(true);
        if (changed && onChange) onChange(id);
        return;
    }
    open = false;
    layout(true);
    if (onOpenChanged) onOpenChanged(false);
    if (changed && onChange) onChange(id);
}

void IconPicker::mouseEnter(const MouseEvent &)
{
    if (alwaysOpen) return;
    stopTimer();
    // owner 2026-11-25: wait a second on hover before the other choices open (a click opens at once)
    if (!open) { pendingOpen = true; startTimer(1000); }
}

void IconPicker::mouseExit(const MouseEvent &)
{
    if (alwaysOpen) return;
    if (!open) { pendingOpen = false; stopTimer(); return; }
    // close a moment after the pointer leaves (moving between circles crosses small gaps)
    startTimer(280);
}

void IconPicker::timerCallback()
{
    stopTimer();
    if (pendingOpen) { pendingOpen = false; if (!open && isMouseOver(true)) setOpen(true); return; }
    if (open && !isMouseOver(true)) setOpen(false);
}

}
