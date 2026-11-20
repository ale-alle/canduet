// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanPlugins.h"
#include "TinCanPaths.h"
#include "TinCanTheme.h"

namespace TinCan {

// what was found, with JUCE's own scanning (search folders, dead-man's pedal, progress)
class PluginHost::ListWindow : public DocumentWindow
{
public:
    ListWindow(PluginHost & host)
        : DocumentWindow(TRANS("Plugins"), TinCanTheme::night, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        setResizeLimits(520, 320, 4000, 4000);
        auto * list = new PluginListComponent(host.formats, host.known,
                                              getDataFolder().getChildFile("plugin-scan-crashed.txt"),
                                              host.settings.get(), false);
        setContentOwned(list, false);
        centreWithSize(760, 520);
    }
    void closeButtonPressed() override { setVisible(false); }
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
