// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanPlugins.h"
#include "TinCanPaths.h"
#include "TinCanPluginScan.h"
#include "TinCanTheme.h"

namespace TinCan {

// T-092: finding plugins goes through a helper per plugin (TinCanPluginScan), so a plugin that
// crashes or hangs while it's checked is put on the failed list instead of closing CanDuet
class PluginHost::Scanner : public KnownPluginList::CustomScanner
{
public:
    explicit Scanner(PluginHost & h) : host(h) {}

    bool findPluginTypesFor(AudioPluginFormat & format, OwnedArray<PluginDescription> & result,
                            const String & fileOrIdentifier) override
    {
        const auto r = PluginScan::scanInHelper(format.getName(), fileOrIdentifier, result,
                                                [this] { return shouldExit(); });
        if (r == PluginScan::Result::ok || r == PluginScan::Result::cancelled) return true;
        if (r == PluginScan::Result::couldNotStart) {
            // the helper couldn't start at all (blocked by security software?): check it here, as before
            format.findAllTypesForFile(result, fileOrIdentifier);
            return true;
        }
        host.noteFailure(fileOrIdentifier, PluginScan::describe(r));
        return false;   // JUCE puts it on the failed list
    }

private:
    PluginHost & host;
};

// what was found, with JUCE's own scanning (search folders, progress), and below it the plugins
// that didn't work, why, and a button to try them again (owner 2026-11-26)
class PluginHost::ListPanel : public Component, private ChangeListener
{
public:
    explicit ListPanel(PluginHost & h)
        : list(h.formats, h.known, getDataFolder().getChildFile("plugin-scan-crashed.txt"), h.settings.get(), false),
          host(h)
    {
        // checks run on a background thread, so the window and Cancel keep working while a helper is busy
        list.setNumberOfThreadsForScanning(1);
        addAndMakeVisible(list);

        note.setColour(Label::textColourId, TinCanTheme::muted);
        note.setJustificationType(Justification::centredLeft);
        note.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(note);

        retry.setButtonText(TRANS("Try failed plugins again"));
        retry.onClick = [this] { host.retryFailed(); };
        addAndMakeVisible(retry);

        host.known.addChangeListener(this);
        refresh();
    }
    ~ListPanel() override { host.known.removeChangeListener(this); }

    void paint(Graphics & g) override { g.fillAll(TinCanTheme::night); }
    void resized() override
    {
        auto r = getLocalBounds();
        auto foot = r.removeFromBottom(76).reduced(12, 8);
        list.setBounds(r);
        retry.setBounds(foot.removeFromRight(220).withSizeKeepingCentre(220, 32));
        foot.removeFromRight(12);
        note.setBounds(foot);
    }

    PluginListComponent list;

private:
    void changeListenerCallback(ChangeBroadcaster *) override { refresh(); }

    void refresh()
    {
        const StringArray failed = host.known.getBlacklistedFiles();
        retry.setEnabled(!failed.isEmpty());
        if (failed.isEmpty()) {
            note.setText(TRANS("Each plugin is checked in a separate helper, so a broken plugin can't close CanDuet."),
                         dontSendNotification);
            return;
        }
        StringArray lines;
        for (auto & f : failed) {
            auto why = host.getFailReason(f);
            if (why.isEmpty()) why = TRANS("didn't work last time");
            lines.add(File::createFileWithoutCheckingPath(f).getFileNameWithoutExtension() + ": " + why);
        }
        note.setText(TRANS("Didn't work") + " (" + String(failed.size()) + ")  " + lines.joinIntoString("   |   "),
                     dontSendNotification);
    }

    PluginHost & host;
    Label note;
    TextButton retry;
};

class PluginHost::ListWindow : public DocumentWindow
{
public:
    ListWindow(PluginHost & host)
        : DocumentWindow(TRANS("Plugins"), TinCanTheme::night, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(520, 360, 4000, 4000);
        panel = new ListPanel(host);
        setContentOwned(panel, false);
        centreWithSize(760, 560);
    }
    void closeButtonPressed() override { setVisible(false); }

    ListPanel * panel = nullptr;   // owned by the window
};

// the plugin's own window; closing it deletes the editor (the plugin keeps playing)
class PluginHost::EditorWindow : public DocumentWindow
{
public:
    EditorWindow(PluginHost & h, AudioPluginInstance & inst, const String & title)
        : DocumentWindow(title, TinCanTheme::night, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          host(h), instance(inst)
    {
        setUsingNativeTitleBar(true);
        AudioProcessorEditor * editor = instance.hasEditor() ? instance.createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new GenericAudioProcessorEditor(instance);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    ~EditorWindow() override
    {
        // the editor must go before the plugin can be deleted (it tells the plugin itself)
        clearContentComponent();
    }
    void closeButtonPressed() override
    {
        auto * inst = &instance;
        auto * hp = &host;
        MessageManager::callAsync([hp, inst]() { hp->closeEditor(inst); });
    }

private:
    PluginHost & host;
    AudioPluginInstance & instance;
};

PluginHost::PluginHost()
{
#if JUCE_PLUGINHOST_VST3
    formats.addFormat(new VST3PluginFormat());
#endif
    PropertiesFile::Options opts;
    opts.applicationName = "plugin-folders";
    opts.filenameSuffix = ".settings";
    opts.folderName = getDataFolderName();
    opts.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<PropertiesFile>(getDataFolder().getChildFile("plugin-folders.settings"), opts);

    if (auto xml = parseXML(getListFile())) known.recreateFromXml(*xml);
    if (auto reasons = settings->getXmlValue("scanFailures"))           // T-092
        for (auto * e : reasons->getChildIterator())
            failReasons[e->getStringAttribute("file")] = e->getStringAttribute("reason");
    known.setCustomScanner(std::make_unique<Scanner>(*this));
    known.addChangeListener(this);
}

PluginHost::~PluginHost()
{
    known.removeChangeListener(this);
    closeAllEditors();
    listWindow.reset();
}

File PluginHost::getListFile() const
{
    return getDataFolder().getChildFile("plugins.xml");
}

void PluginHost::changeListenerCallback(ChangeBroadcaster *)
{
    // the list changed (a search, or something removed): keep it for next time
    if (auto xml = known.createXml()) {
        getDataFolder().createDirectory();
        xml->writeTo(getListFile());
    }
    saveFailReasons();
}

void PluginHost::noteFailure(const String & fileOrIdentifier, const String & reason)
{
    const std::lock_guard<std::mutex> lock(failLock);
    failReasons[fileOrIdentifier] = reason;
}

String PluginHost::getFailReason(const String & fileOrIdentifier) const
{
    const std::lock_guard<std::mutex> lock(failLock);
    auto it = failReasons.find(fileOrIdentifier);
    return it != failReasons.end() ? it->second : String();
}

void PluginHost::saveFailReasons()
{
    // message thread: save the reasons of plugins on the failed list (a reason is only shown for those,
    // so older ones are just left in memory; a new check overwrites them)
    const StringArray failed = known.getBlacklistedFiles();
    XmlElement xml("FAILURES");
    {
        const std::lock_guard<std::mutex> lock(failLock);
        for (auto & [file, reason] : failReasons) {
            if (!failed.contains(file)) continue;
            auto * e = xml.createNewChildElement("PLUGIN");
            e->setAttribute("file", file);
            e->setAttribute("reason", reason);
        }
    }
    settings->setValue("scanFailures", &xml);
    settings->saveIfNeeded();
}

void PluginHost::retryFailed()
{
    const StringArray failed = known.getBlacklistedFiles();
    if (failed.isEmpty() || listWindow == nullptr || listWindow->panel->list.isScanning()) return;
    known.clearBlacklistedFiles();   // off the list; one that fails again gets its new reason
    for (auto * f : formats.getFormats())
        listWindow->panel->list.scanFor(*f, failed);
}

static Array<PluginDescription> sortedByName(Array<PluginDescription> list)
{
    std::sort(list.begin(), list.end(), [](const PluginDescription & a, const PluginDescription & b) {
        return a.name.compareNatural(b.name) < 0;
    });
    return list;
}

Array<PluginDescription> PluginHost::getInstruments() const
{
    Array<PluginDescription> out;
    for (auto & d : known.getTypes()) if (d.isInstrument) out.add(d);
    return sortedByName(out);
}

Array<PluginDescription> PluginHost::getEffects() const
{
    Array<PluginDescription> out;
    for (auto & d : known.getTypes()) if (!d.isInstrument) out.add(d);
    return sortedByName(out);
}

std::unique_ptr<PluginDescription> PluginHost::findByIdentifier(const String & id) const
{
    for (auto & d : known.getTypes())
        if (d.createIdentifierString() == id) return std::make_unique<PluginDescription>(d);
    return nullptr;
}

std::unique_ptr<AudioPluginInstance> PluginHost::create(const PluginDescription & description, double sampleRate,
                                                        int blockSize, String & error)
{
    error.clear();
    auto inst = formats.createPluginInstance(description, sampleRate > 0 ? sampleRate : 48000.0,
                                             blockSize > 0 ? blockSize : 512, error);
    if (!inst) {
        if (error.isEmpty()) error = TRANS("The plugin didn't start.");
        return nullptr;
    }
    // stereo in (effects) and out where the plugin can do it; otherwise whatever it wants
    auto layout = inst->getBusesLayout();
    bool changed = false;
    if (layout.outputBuses.size() > 0 && layout.outputBuses[0] != AudioChannelSet::stereo()) {
        layout.outputBuses.getReference(0) = AudioChannelSet::stereo();
        changed = true;
    }
    if (!description.isInstrument && layout.inputBuses.size() > 0 && layout.inputBuses[0] != AudioChannelSet::stereo()) {
        layout.inputBuses.getReference(0) = AudioChannelSet::stereo();
        changed = true;
    }
    if (changed && inst->checkBusesLayoutSupported(layout)) inst->setBusesLayout(layout);
    inst->enableAllBuses();
    return inst;
}

void PluginHost::showPluginList()
{
    if (!listWindow) listWindow = std::make_unique<ListWindow>(*this);
    listWindow->setVisible(true);
    listWindow->toFront(true);
}

void PluginHost::showEditor(AudioPluginInstance & instance, const String & title)
{
    auto it = editors.find(&instance);
    if (it != editors.end()) {
        it->second->setVisible(true);
        if (it->second->isMinimised()) it->second->setMinimised(false);
        it->second->toFront(true);
        return;
    }
    editors[&instance] = std::make_unique<EditorWindow>(*this, instance, title);
}

void PluginHost::closeEditor(AudioPluginInstance * instance)
{
    editors.erase(instance);
}

void PluginHost::closeAllEditors()
{
    editors.clear();
}

String PluginHost::saveState(AudioPluginInstance & instance)
{
    MemoryBlock block;
    instance.getStateInformation(block);
    return block.toBase64Encoding();
}

void PluginHost::restoreState(AudioPluginInstance & instance, const String & state)
{
    if (state.isEmpty()) return;
    MemoryBlock block;
    if (block.fromBase64Encoding(state) && block.getSize() > 0)
        instance.setStateInformation(block.getData(), (int) block.getSize());
}

}
