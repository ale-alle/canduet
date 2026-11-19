// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell


#include "CrossPlatformUtils.h"

#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_WINDOWS

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "DebugLogC.h"

void getSafeAreaInsets(void * component, float & top, float & bottom, float & left, float & right)
{
    top = bottom = left = right = 0;
}

// ---- CanDuet (T-077): less interference with the audio thread ----
// Declared here rather than taken from the SDK headers, which hide them behind _WIN32_WINNT, and looked
// up at run time, so older Windows simply skips them.
namespace {
    struct PowerThrottlingState { unsigned long Version, ControlMask, StateMask; };
    constexpr int kProcessPowerThrottling = 4;          // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
    constexpr int kThreadPowerThrottling = 3;           // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
    constexpr unsigned long kThrottleExecutionSpeed = 0x1;
    constexpr unsigned long kThrottleIgnoreTimerResolution = 0x4;   // process only

    using SetInfoFn = int (__stdcall *)(void *, int, void *, unsigned long);
    using AvSetMmFn = void * (__stdcall *)(const wchar_t *, unsigned long *);

    SetInfoFn setThreadInformation = nullptr;
    AvSetMmFn avSetMmThreadCharacteristics = nullptr;
}

void reduceWindowsAudioInterference()
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel) {
        if (auto setProcessInformation = (SetInfoFn) GetProcAddress(kernel, "SetProcessInformation")) {
            // control these, state 0: never throttle execution speed, keep the timer resolution we ask for
            PowerThrottlingState s { 1, kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution, 0 };
            setProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof(s));
        }
        setThreadInformation = (SetInfoFn) GetProcAddress(kernel, "SetThreadInformation");
    }
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    if (HMODULE avrt = LoadLibraryW(L"avrt.dll"))   // kept loaded for the app's life
        avSetMmThreadCharacteristics = (AvSetMmFn) GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
}

void markAudioThreadForProAudio()
{
    // only calls functions looked up at start: nothing to load or allocate on the audio thread
    if (avSetMmThreadCharacteristics) {
        unsigned long taskIndex = 0;
        avSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    }
    if (setThreadInformation) {
        PowerThrottlingState s { 1, kThrottleExecutionSpeed, 0 };
        setThreadInformation(GetCurrentThread(), kThreadPowerThrottling, &s, sizeof(s));
    }
}

#endif
