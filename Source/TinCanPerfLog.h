// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"

// T-091 (Phase 0 perf kit, doc/PERF-TEST.md): test builds only (CMake TINCAN_PERF_LOG=ON).
// Once a second, on the message thread, writes one CSV line to %APPDATA%\<data folder>\perf\:
// audio load (average + peak), dropouts, and per connected person the latency, jitter, buffer
// and packet counts. Reads only what the app's own screens already read; never touches audio.
// tools/perf-report.ps1 turns a log into a PASS/FAIL summary.

#ifndef TINCAN_PERF_LOG
 #define TINCAN_PERF_LOG 0
#endif

#if TINCAN_PERF_LOG

class SonobusAudioProcessor;

namespace TinCan {

class PerfLog : private Timer
{
public:
    static constexpr int maxPeers = 3;      // columns kept for the first three connected people
    static constexpr int keepFiles = 20;    // older logs are deleted when a new one starts

    explicit PerfLog (SonobusAudioProcessor & p);
    ~PerfLog() override;

    static String headerLine();

private:
    void timerCallback() override;
    void openFile();

    SonobusAudioProcessor & processor;
    std::unique_ptr<FileOutputStream> out;
    uint32 startMs = 0;
};

}

#endif
