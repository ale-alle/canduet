// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

#include <map>
#include <memory>

// VST3 plugins (T-086). Teachers and students load VST3 instruments (for the score now) and effects
// (on the mixer's strips, next). Plugins run inside CanDuet: a broken plugin can take the app down,
// so finding plugins uses JUCE's dead-man's-pedal file to skip one that crashed the last search.
// VST3 only: Steinberg no longer licenses VST2 to new hosts.
namespace TinCan {

class PluginHost : private ChangeListener
{
public:
    PluginHost();
    ~PluginHost() override;

    AudioPluginFormatManager & getFormats() { return formats; }
    KnownPluginList & getKnown() { return known; }

    // the plugins found so far, instruments or effects, sorted by name
    Array<PluginDescription> getInstruments() const;
    Array<PluginDescription> getEffects() const;
    // the one with this identifier (PluginDescription::createIdentifierString), if it was found
    std::unique_ptr<PluginDescription> findByIdentifier(const String & id) const;

    // message thread: a ready-to-prepare instance, stereo out where the plugin allows; nullptr with error
    std::unique_ptr<AudioPluginInstance> create(const PluginDescription & description, double sampleRate,
                                                int blockSize, String & error);

    // the "Plugins" window: what was found, a button to search the VST3 folders, folders to add
    void showPluginList();

    // the plugin's own window (its knobs and sounds); one per instance, closed before it is deleted
    void showEditor(AudioPluginInstance & instance, const String & title);
    void closeEditor(AudioPluginInstance * instance);
    void closeAllEditors();

    // plugin state as text for the settings, and back
    static String saveState(AudioPluginInstance & instance);
    static void restoreState(AudioPluginInstance & instance, const String & state);

private:
    class ListWindow;
    class EditorWindow;

    void changeListenerCallback(ChangeBroadcaster *) override;
    File getListFile() const;

    AudioPluginFormatManager formats;
    KnownPluginList known;
    std::unique_ptr<PropertiesFile> settings;   // search folders for the Plugins window
    std::unique_ptr<ListWindow> listWindow;
    std::map<AudioPluginInstance *, std::unique_ptr<EditorWindow>> editors;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

}
