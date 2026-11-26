// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanPluginScan.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace TinCan::PluginScan {

bool isHelper (const StringArray & args)
{
    return args.contains (helperFlag);
}

int runHelper (const StringArray & args)
{
#if JUCE_WINDOWS
    // a plugin that crashes here must not put a Windows "stopped working" box on the screen
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    const int at = args.indexOf (helperFlag);
    if (at < 0 || at + 3 >= args.size()) return 2;
    const String formatName = args[at + 1];
    const String identifier = args[at + 2];
    const File resultFile (args[at + 3]);

    AudioPluginFormatManager formats;
#if JUCE_PLUGINHOST_VST3
    formats.addFormat (new VST3PluginFormat());
#endif
    AudioPluginFormat * format = nullptr;
    for (auto * f : formats.getFormats())
        if (f->getName() == formatName) format = f;
    if (format == nullptr) return 3;

    OwnedArray<PluginDescription> found;
    format->findAllTypesForFile (found, identifier);   // if the plugin crashes, it's here

    XmlElement list ("LIST");
    for (auto * d : found) list.addChildElement (d->createXml().release());
    return list.writeTo (resultFile) ? 0 : 4;
}

Result scanInHelper (const String & formatName, const String & fileOrIdentifier,
                     OwnedArray<PluginDescription> & found, const std::function<bool()> & shouldStop,
                     int timeout)
{
    const auto exe = File::getSpecialLocation (File::currentExecutableFile);
    const auto resultFile = File::getSpecialLocation (File::tempDirectory)
                                .getNonexistentChildFile ("canduet-plugin-scan", ".xml", false);

    ChildProcess helper;
    if (! helper.start (StringArray { exe.getFullPathName(), helperFlag, formatName, fileOrIdentifier,
                                      resultFile.getFullPathName() }, 0))
        return Result::couldNotStart;

    const auto started = Time::getMillisecondCounter();
    while (helper.isRunning()) {
        if (shouldStop && shouldStop()) {
            helper.kill();
            resultFile.deleteFile();
            return Result::cancelled;
        }
        if (Time::getMillisecondCounter() - started > (uint32) timeout) {
            helper.kill();
            resultFile.deleteFile();
            return Result::timedOut;
        }
        Thread::sleep (25);
    }

    // the file is only written once the plugin was read; a crash after that (while the helper
    // closes) doesn't matter, the result is still good
    auto xml = parseXML (resultFile);
    resultFile.deleteFile();
    if (xml == nullptr || ! xml->hasTagName ("LIST")) return Result::crashed;

    for (auto * item : xml->getChildIterator()) {
        auto d = std::make_unique<PluginDescription>();
        if (d->loadFromXml (*item)) found.add (std::move (d));
    }
    return Result::ok;
}

String describe (Result r)
{
    switch (r) {
        case Result::crashed:       return TRANS ("crashed while being checked");
        case Result::timedOut:      return TRANS ("took longer than 30 seconds");
        case Result::couldNotStart: return TRANS ("couldn't be checked");
        case Result::cancelled:
        case Result::ok:            break;
    }
    return {};
}

}
