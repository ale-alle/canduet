// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// CanDuet: the shared Delay that mixer strips send to (T-084). Stereo, wet only (it's a send bus):
// a delay line per side with feedback through a gentle low-pass, so each repeat gets a bit darker.
// Settings are read once per block; the time glides to a new value instead of jumping (no clicks).

#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace TinCan {

class Delay
{
public:
    static constexpr float maxTimeMs = 2000.0f;

    // settings, safe to change from any thread
    std::atomic<float> timeMs { 350.0f };     // 20 .. 2000
    std::atomic<float> feedback { 0.35f };    // 0 .. 0.9 (how many repeats)
    std::atomic<float> toneHz { 6000.0f };    // low-pass on the repeats, 1000 .. 16000
    std::atomic<float> level { 0.8f };        // output, 0 .. 1

    void prepare (double sampleRate)
    {
        rate = sampleRate > 0 ? sampleRate : 48000.0;
        size = (int) std::ceil (rate * (maxTimeMs / 1000.0f + 0.05f)) + 4;
        for (auto & line : lines) line.assign ((size_t) size, 0.0f);
        lp[0] = lp[1] = 0.0f;
        pos = 0;
        curDelay = clampTime (timeMs.load()) * (float) rate / 1000.0f;
        curLevel = level.load();
    }

    void reset()
    {
        for (auto & line : lines) std::fill (line.begin(), line.end(), 0.0f);
        lp[0] = lp[1] = 0.0f;
    }

    // io: the summed sends in channels 0 (and 1); replaced by the delayed signal
    void process (AudioBuffer<float> & io, int numSamples)
    {
        if (size == 0 || io.getNumChannels() < 1) return;
        float * ch[2] = { io.getWritePointer (0), io.getWritePointer (io.getNumChannels() > 1 ? 1 : 0) };
        const bool stereo = io.getNumChannels() > 1;

        const float targetDelay = clampTime (timeMs.load()) * (float) rate / 1000.0f;
        const float fb = jlimit (0.0f, 0.9f, feedback.load());
        const float cutoff = jlimit (500.0f, 18000.0f, toneHz.load());
        const float a = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * cutoff / (float) rate);
        const float targetLevel = jlimit (0.0f, 1.0f, level.load());
        const float levelStep = (targetLevel - curLevel) / (float) jmax (1, numSamples);
        const float glide = 1.0f - std::exp (-1.0f / (0.08f * (float) rate));   // ~80 ms to a new time

        for (int i = 0; i < numSamples; ++i) {
            curDelay += (targetDelay - curDelay) * glide;
            curLevel += levelStep;
            float rp = (float) pos - curDelay;
            while (rp < 0.0f) rp += (float) size;
            const int i0 = (int) rp;
            const int i1 = (i0 + 1) % size;
            const float frac = rp - (float) i0;

            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                auto & line = lines[c];
                const float delayed = line[(size_t) i0] + frac * (line[(size_t) i1] - line[(size_t) i0]);
                lp[c] += a * (delayed - lp[c]);
                float in = ch[c][i];
                float w = in + lp[c] * fb;
                if (! std::isfinite (w)) w = 0.0f;
                line[(size_t) pos] = w;
                ch[c][i] = delayed * curLevel;
            }
            if (++pos >= size) pos = 0;
        }
        curLevel = targetLevel;
    }

private:
    static float clampTime (float ms) { return jlimit (20.0f, maxTimeMs, ms); }

    double rate = 48000.0;
    int size = 0, pos = 0;
    std::vector<float> lines[2];
    float lp[2] { 0.0f, 0.0f };
    float curDelay = 0.0f, curLevel = 0.8f;
};

} // namespace TinCan
