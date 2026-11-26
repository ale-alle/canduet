// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

#include <functional>

// T-092 safe plugin scan: each plugin is checked in its own short-lived helper
// (CanDuet.exe --canduet-scan-plugin <format> <file> <result.xml>). If the plugin crashes or hangs,
// only the helper goes: CanDuet marks it "didn't work" and carries on. Owner 2026-11-26: 30 s limit.
//
// This covers finding plugins. A plugin that is used on a strip still runs inside CanDuet
// (running it in a helper would add latency), so playing it is not protected.
namespace TinCan::PluginScan {

constexpr const char * helperFlag = "--canduet-scan-plugin";
constexpr int timeoutMs = 30000;

// helper side (CanDuet started with helperFlag): scans, writes the result file, returns the exit code
bool isHelper (const StringArray & args);
int runHelper (const StringArray & args);

enum class Result { ok, crashed, timedOut, cancelled, couldNotStart };

// app side, any thread but the audio thread: blocks until the helper is done (at most timeoutMs);
// shouldStop is polled so Cancel works
Result scanInHelper (const String & formatName, const String & fileOrIdentifier,
                     OwnedArray<PluginDescription> & found, const std::function<bool()> & shouldStop,
                     int timeout = timeoutMs);

// what the Plugins window says about a failure
String describe (Result r);

}
