// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanHomework.h"
#include "TinCanDock.h"
#include "TinCanJoin.h"
#include "TinCanTheme.h"
#include "TinCanVideo.h"

namespace TinCan {

// stays on the site; any other link opens in the normal browser
class HomeworkPanel::Browser : public WebBrowserComponent
{
public:
    Browser(const Options & options, const String & origin_) : WebBrowserComponent(options), origin(origin_) {}

    bool pageAboutToLoad(const String & newURL) override
    {
        if (newURL.startsWith(origin + "/") || newURL == origin || newURL == "about:blank") return true;
        if (newURL.startsWithIgnoreCase("https://")) URL(newURL).launchInDefaultBrowser();
        return false;
    }
    void newWindowAttemptingToLoad(const String & newURL) override
    {
        if (newURL.startsWithIgnoreCase("https://")) URL(newURL).launchInDefaultBrowser();
    }

private:
    String origin;
};

static String originOf(const String & url)
{
    const auto scheme = url.upToFirstOccurrenceOf("://", true, false);
    const auto rest = url.fromFirstOccurrenceOf("://", false, false);
    return scheme + rest.upToFirstOccurrenceOf("/", false, false);
}

HomeworkPanel::HomeworkPanel(SummaryHooks summaryHooks, int64 lessonStartMs)
{
    const auto base = getApiBase().trimCharactersAtEnd("/");
    homeworkUrl = base + "/homework?app=canduet" + (lessonStartMs > 0 ? "&starts=" + String(lessonStartMs) : String());

    auto options = makeWebViewOptions();
    if (WebBrowserComponent::areOptionsSupported(options)) {
        browser = std::make_unique<Browser>(options, originOf(base));
        addChildComponent(browser.get());
    }
    noBrowser.setText(TRANS("Homework needs Microsoft Edge WebView2, which isn't installed on this computer. "
                            "You can see it on My lessons at tincanstudios.ca."), dontSendNotification);
    noBrowser.setJustificationType(Justification::centred);
    noBrowser.setColour(Label::textColourId, TinCanTheme::muted);
    addChildComponent(noBrowser);

    recordings = std::make_unique<SummariesPanel>(std::move(summaryHooks));
    // its icons start after the two section icons, and stop before the dock icons
    recordings->getProperties().set("railLeft", 8 + 2 * IconCircle::size + 6 + 4 + 14);
    recordings->getProperties().set("railRight", DockHolder::cornerWidth);
    addChildComponent(recordings.get());

    sections.setOptions({ { 0, Icons::homework(), {}, {}, TRANS("Homework") },
                          { 1, Icons::recording(), {}, {}, TRANS("Recordings and highlights from this computer") } }, 0);
    sections.setAlwaysOpen(true);
    sections.onChange = [this](int id) { showSection(id); };
    addAndMakeVisible(sections);
    showSection(0);
}

HomeworkPanel::~HomeworkPanel() = default;

void HomeworkPanel::showSection(int index)
{
    sections.setSelected(index);
    const bool hw = index == 0;
    if (hw && browser && !loaded) { browser->goToURL(homeworkUrl); loaded = true; }
    if (browser) browser->setVisible(hw);
    noBrowser.setVisible(hw && !browser);
    recordings->setVisible(!hw);
    if (!hw) recordings->refresh();
    sections.toFront(false);
}

void HomeworkPanel::showHomework() { showSection(0); }

void HomeworkPanel::showRecordings(const File & select)
{
    showSection(1);
    recordings->refresh(select);
}

void HomeworkPanel::refreshRecordings() { recordings->refresh(); }

void HomeworkPanel::paint(Graphics & g)
{
    g.fillAll(TinCanTheme::surface);
    g.setColour(TinCanTheme::line);
    g.fillRect(0, ToolRail::height - 1, getWidth(), 1);
}

void HomeworkPanel::resized()
{
    const int d = IconCircle::size + 4;
    sections.setBounds(8, (ToolRail::height - d) / 2, sections.getOpenWidth(), d);
    recordings->setBounds(getLocalBounds());
    auto below = getLocalBounds().withTrimmedTop(ToolRail::height);
    if (browser) browser->setBounds(below);
    noBrowser.setBounds(below.reduced(24));
    sections.toFront(false);
}

}
