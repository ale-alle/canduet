// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// The perf kit's peak audio load (T-091). Part of TinCanAudioTests.

#include <JuceHeader.h>
#include "../Source/TinCanPerfPeak.h"

#include <cmath>
#include <cstdio>
#include <thread>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int runPerfPeakTests()
{
    const int before = failures;
    const double rate = 48000.0;

    // not prepared yet: nothing is recorded
    {
        TinCan::PeakLoad p;
        p.record(0.01, 480);
        CHECK(p.takePeak() == 0.0f);
    }

    // 256 samples at 48 kHz = 5.333 ms: 2.667 ms is half the block, 8 ms is 1.5 blocks; the peak keeps the slowest
    {
        TinCan::PeakLoad p;
        p.prepare(rate);
        p.record(256.0 / rate * 0.25, 256);
        p.record(256.0 / rate * 0.5, 256);
        p.record(256.0 / rate * 0.1, 256);
        CHECK(std::abs(p.takePeak() - 0.5f) < 1.0e-4f);
        CHECK(p.takePeak() == 0.0f);                      // taking it resets
        p.record(256.0 / rate * 1.5, 256);
        CHECK(std::abs(p.takePeak() - 1.5f) < 1.0e-4f);
        p.record(0.01, 0);                                // a zero-length block is ignored
        CHECK(p.takePeak() == 0.0f);
    }

    // the scoped timer measures real time: sleep ~10 ms in a "block" of 480 samples (10 ms) -> about 1.0
    {
        TinCan::PeakLoad p;
        p.prepare(rate);
        {
            TinCan::PeakLoad::ScopedTimer t(p, 480);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const float pk = p.takePeak();
        CHECK(pk > 0.9f && pk < 5.0f);                    // sleep can overshoot on Windows; never under
    }

    // two threads recording at once keep the true maximum
    {
        TinCan::PeakLoad p;
        p.prepare(rate);
        auto work = [&p] (float top) { for (int i = 1; i <= 20000; ++i) p.record(256.0 / 48000.0 * top * i / 20000.0, 256); };
        std::thread a(work, 0.7f), b(work, 0.9f);
        a.join(); b.join();
        CHECK(std::abs(p.takePeak() - 0.9f) < 1.0e-4f);
    }

    if (failures == before) std::printf("PerfPeak: ok\n");
    return failures - before;
}
