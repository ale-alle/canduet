// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanWorkspace.h"
#include "TinCanPaths.h"
#include "TinCanTheme.h"
#include "TinCanVideo.h"
#include "BinaryData.h"

namespace TinCan {

String getWorkspacePageURL(const String & displayName)
{
    auto dev = SystemStats::getEnvironmentVariable("TINCAN_WORKSPACE_PAGE", {}).trim();
    File index;
    if (dev.isNotEmpty() && File::isAbsolutePath(dev) && File(dev).getChildFile("index.html").existsAsFile()) {
        index = File(dev).getChildFile("index.html");
    }
    else {
        auto folder = TinCan::getDataFolder().getChildFile("workspace");
        folder.createDirectory();
        const struct { const char * name; const char * data; int size; } files[] = {
            { "index.html", BinaryData::index_html, BinaryData::index_htmlSize },
            { "workspace-model.js", BinaryData::workspacemodel_js, BinaryData::workspacemodel_jsSize },
            { "workspace-ui.js", BinaryData::workspaceui_js, BinaryData::workspaceui_jsSize },
        };
        for (auto & f : files) {
            folder.getChildFile(f.name).replaceWithData(f.data, (size_t) f.size);
        }
        index = folder.getChildFile("index.html");
    }
    // the fragment stays on this computer; the page reads role and name from it
    return URL(index).toString(false) + "#role=teacher&name=" + URL::addEscapeChars(displayName.substring(0, 80), true);
}

#if JUCE_WEB_BROWSER && JUCE_USE_WIN_WEBVIEW2

namespace {

// Stays on the workspace page; links and new windows never open.
class WorkspaceBrowser : public WebBrowserComponent
{
public:
    WorkspaceBrowser(const Options & options, const String & page_, std::function<void(const var &)> onMessage_)
        : WebBrowserComponent(options), page(page_), onMessage(std::move(onMessage_)) {}

    bool pageAboutToLoad(const String & newURL) override
    {
        return newURL.upToFirstOccurrenceOf("#", false, false) == page || newURL == "about:blank";
    }

    void newWindowAttemptingToLoad(const String &) override {}

    void webMessageReceived(const String & sourceURL, const String & message) override
    {
        if (sourceURL.upToFirstOccurrenceOf("#", false, false) != page || message.length() > 4096) return;
        var m;
        if (JSON::parse(message, m).failed() || !m.isObject()) return;
        if (onMessage) onMessage(m);
    }

private:
    String page;
    std::function<void(const var &)> onMessage;
};

}

WorkspaceWindow::WorkspaceWindow(const String & displayName)
    : DockableWindow(TRANS("Board"), 1200, 800)
{
    auto options = makeWebViewOptions();
    if (WebBrowserComponent::areOptionsSupported(options)) {
        auto url = getWorkspacePageURL(displayName);
        auto b = std::make_unique<WorkspaceBrowser>(options, url.upToFirstOccurrenceOf("#", false, false),
                                                    [this](const var & m) { handleMessage(m); });
        b->goToURL(url);
        browser = b.get();
        setBody(std::move(b));
        getHolder().setControlsInBody([this](bool docked, bool canDock) { postDock(docked, canDock); });
    }
    else {
        auto label = std::make_unique<Label>(String(), TRANS("The workspace needs Microsoft Edge WebView2, which isn't installed on this computer."));
        label->setJustificationType(Justification::centred);
        setBody(std::move(label));
    }
}

void WorkspaceWindow::handleMessage(const var & m)
{
    const auto type = m.getProperty("t", var()).toString();
    if (type == "ready") {
        ready = true;
        postDock(getHolder().isDocked(), getHolder().canDockIn());
    }
    else if (type == "dockToggle") getHolder().requestToggleDock();
    else if (type == "dockClose") {
        if (getHolder().isDocked()) getHolder().requestClose();
        else closeButtonPressed();
    }
}

void WorkspaceWindow::postDock(bool docked, bool canDock)
{
    if (browser == nullptr || !ready) return;
    DynamicObject::Ptr m = new DynamicObject();
    m->setProperty("t", "dock");
    m->setProperty("docked", docked);
    m->setProperty("canDock", canDock);
    browser->postWebMessage(JSON::toString(var(m.get()), true));
}

#else

WorkspaceWindow::WorkspaceWindow(const String &)
    : DockableWindow(TRANS("Board"), 480, 240)
{
    auto label = std::make_unique<Label>(String(), TRANS("The workspace isn't available in this build."));
    label->setJustificationType(Justification::centred);
    setBody(std::move(label));
}

void WorkspaceWindow::handleMessage(const var &) {}
void WorkspaceWindow::postDock(bool, bool) {}

#endif

WorkspaceWindow::~WorkspaceWindow() = default;

}
