// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// The shared Delay the mixer strips send to (T-084). Part of TinCanAudioTests.

#include <JuceHeader.h>
#include "../Source/TinCanDelay.h"

#include <cmath>
#include <cstdio>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// peak position and value in [from, to)
static std::pair<int, float> peak(const std::vector<float> & v, int from, int to)
{
    int at = from; float best = 0.0f;
    for (int i = from; i < to && i < (int) v.size(); ++i) if (std::abs(v[(size_t) i]) > best) { best = std::abs(v[(size_t) i]); at = i; }
    return { at, best };
}

int runDelayTests()
{
    const int before = failures;
    const double rate = 48000.0;
    TinCan::Delay d;
    d.timeMs = 250.0f; d.feedback = 0.5f; d.toneHz = 16000.0f; d.level = 1.0f;
    d.prepare(rate);

    // an impulse, then silence, in 512-sample blocks: echoes at 250, 500, 750 ms, each about half the last
    const int total = (int) rate * 2;
    std::vector<float> out((size_t) total, 0.0f);
    AudioBuffer<float> buf(2, 512);
    for (int done = 0; done < total; done += 512) {
        buf.clear();
        if (done == 0) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); }
        d.process(buf, 512);
        for (int i = 0; i < 512 && done + i < total; ++i) out[(size_t) (done + i)] = buf.getSample(0, i);
    }
    const int t = (int) (rate * 0.25);
    CHECK(peak(out, 0, t - 100).second < 1e-4f);   // nothing before the first echo (wet only)
    auto e1 = peak(out, t - 100, t + 100), e2 = peak(out, 2 * t - 100, 2 * t + 100), e3 = peak(out, 3 * t - 100, 3 * t + 100);
    CHECK(std::abs(e1.first - t) <= 2 && e1.second > 0.8f);
    CHECK(std::abs(e2.first - 2 * t) <= 2 && e2.second > 0.3f && e2.second < 0.6f * e1.second + 0.05f);
    CHECK(std::abs(e3.first - 3 * t) <= 2 && e3.second < e2.second);

    // no repeats: one echo only
    d.feedback = 0.0f; d.reset();
    std::vector<float> one((size_t) total, 0.0f);
    for (int done = 0; done < total; done += 512) {
        buf.clear();
        if (done == 0) buf.setSample(0, 0, 1.0f);
        d.process(buf, 512);
        for (int i = 0; i < 512 && done + i < total; ++i) one[(size_t) (done + i)] = buf.getSample(0, i);
    }
    CHECK(peak(one, t - 100, t + 100).second > 0.8f);
    CHECK(peak(one, 2 * t - 100, 2 * t + 100).second < 1e-4f);

    std::printf("  shared delay: %s (echoes %.2f, %.2f, %.2f)\n", failures == before ? "ok" : "FAILED", e1.second, e2.second, e3.second);
    return failures;
}
