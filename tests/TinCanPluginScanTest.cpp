// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// Safe plugin scan (T-092): each plugin is checked in a helper, which here is this test exe itself
// (main() runs the helper when started with --canduet-scan-plugin). Part of TinCanAudioTests.
//
// On this computer: TINCAN_TEST_VST3 (a working .vst3, else Surge XT where its installer puts it),
// and optionally TINCAN_TEST_CRASH_VST3 / TINCAN_TEST_HANG_VST3: plugins that crash / hang on load.

#include <JuceHeader.h>
#include "../Source/TinCanPluginScan.h"

#include <cstdio>

using namespace juce;
using ScanResult = TinCan::PluginScan::Result;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static ScanResult scan(const String & file, OwnedArray<PluginDescription> & found, int timeout = 30000,
                   std::function<bool()> stop = {})
{
    return TinCan::PluginScan::scanInHelper("VST3", file, found, stop, timeout);
}

int runPluginScanTests()
{
    const int before = failures;
#if JUCE_PLUGINHOST_VST3
    // a working plugin: found, same as checking it in-process
    {
        File plugin(SystemStats::getEnvironmentVariable("TINCAN_TEST_VST3",
                    "C:\\Program Files\\Common Files\\VST3\\Surge Synth Team\\Surge XT.vst3"));
        if (plugin.exists()) {
            OwnedArray<PluginDescription> found;
            CHECK(scan(plugin.getFullPathName(), found) == ScanResult::ok);
            CHECK(found.size() >= 1);
            if (found.size() > 0) CHECK(found[0]->name.isNotEmpty());
            std::printf("  plugin scan (%s): %s\n", plugin.getFileNameWithoutExtension().toRawUTF8(),
                        found.size() > 0 ? found[0]->name.toRawUTF8() : "nothing found");
        } else {
            std::printf("  plugin scan: working plugin skipped (no %s)\n", plugin.getFullPathName().toRawUTF8());
        }
    }

    // a file that isn't a plugin: checked fine, nothing found (not a failure)
    {
        auto junk = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("not-a-plugin", ".vst3", false);
        junk.replaceWithText("not a plugin");
        OwnedArray<PluginDescription> found;
        CHECK(scan(junk.getFullPathName(), found) == ScanResult::ok);
        CHECK(found.isEmpty());
        junk.deleteFile();
    }

    // a plugin that crashes while it loads: only the helper goes
    {
        File plugin(SystemStats::getEnvironmentVariable("TINCAN_TEST_CRASH_VST3", {}));
        if (plugin.existsAsFile()) {
            OwnedArray<PluginDescription> found;
            CHECK(scan(plugin.getFullPathName(), found) == ScanResult::crashed);
            CHECK(found.isEmpty());
            std::printf("  plugin scan: crashing plugin marked as crashed\n");
        }
    }

    // a plugin that hangs: stopped at the time limit (3 s here), and Cancel stops it sooner
    {
        File plugin(SystemStats::getEnvironmentVariable("TINCAN_TEST_HANG_VST3", {}));
        if (plugin.existsAsFile()) {
            OwnedArray<PluginDescription> found;
            auto t0 = Time::getMillisecondCounter();
            CHECK(scan(plugin.getFullPathName(), found, 3000) == ScanResult::timedOut);
            auto took = Time::getMillisecondCounter() - t0;
            CHECK(took >= 3000 && took < 6000);

            t0 = Time::getMillisecondCounter();
            CHECK(scan(plugin.getFullPathName(), found, 30000,
                       [t0] { return Time::getMillisecondCounter() - t0 > 500; }) == ScanResult::cancelled);
            CHECK(Time::getMillisecondCounter() - t0 < 3000);
            std::printf("  plugin scan: hanging plugin stopped after %u ms\n", (unsigned) took);
        }
    }
#else
    std::printf("  plugin scan: skipped (no VST3 hosting in this build)\n");
#endif
    if (failures == before) std::printf("  plugin scan: ok\n");
    return failures - before;
}
