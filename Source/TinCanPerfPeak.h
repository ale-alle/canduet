// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include <atomic>

// T-091 (Phase 0 perf kit, doc/PERF-TEST.md): the slowest audio block since the last read, as a
// share of the time that block had (1.0 = the block took all of its time = a dropout is likely).
// The average CPU meter (AudioProcessLoadMeasurer) hides short spikes; dropouts come from spikes.
//
// Audio-thread safe: record() is two clock reads and a lock-free atomic max. No allocation, no locks.
// takePeak() is for any other thread and resets the peak.

namespace TinCan {

class PeakLoad
{
public:
    void prepare (double sampleRate) noexcept { mSampleRate.store (sampleRate, std::memory_order_relaxed); }

    void record (double seconds, int numSamples) noexcept
    {
        const double sr = mSampleRate.load (std::memory_order_relaxed);
        if (sr <= 0.0 || numSamples <= 0) return;
        const float load = (float) (seconds * sr / (double) numSamples);
        float cur = mPeak.load (std::memory_order_relaxed);
        while (load > cur && ! mPeak.compare_exchange_weak (cur, load, std::memory_order_relaxed)) {}
    }

    float takePeak() noexcept { return mPeak.exchange (0.0f, std::memory_order_relaxed); }

    struct ScopedTimer
    {
        ScopedTimer (PeakLoad & p, int numSamples) noexcept
            : owner (p), samples (numSamples), start (Time::getHighResolutionTicks()) {}
        ~ScopedTimer() noexcept
        {
            owner.record (Time::highResolutionTicksToSeconds (Time::getHighResolutionTicks() - start), samples);
        }
        PeakLoad & owner;
        int samples;
        int64 start;
        JUCE_DECLARE_NON_COPYABLE (ScopedTimer)
    };

private:
    std::atomic<double> mSampleRate { 0.0 };
    std::atomic<float> mPeak { 0.0f };
};

}
