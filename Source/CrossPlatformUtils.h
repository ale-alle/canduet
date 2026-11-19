// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell


#pragma once

void getSafeAreaInsets(void * component, float & top, float & bottom, float & left, float & right);


#if JUCE_MAC

void disableAppNap();

#endif

#if JUCE_WINDOWS

// CanDuet (T-077): keep Windows from slowing the audio down. At app start: opt the process out of power
// throttling (EcoQoS, which on hybrid CPUs moves threads to efficiency cores), raise its priority and
// load the MMCSS functions. Then, once on each audio callback thread: "Pro Audio" scheduling and no
// throttling for that thread. Both are no-ops where Windows doesn't have them.
void reduceWindowsAudioInterference();
void markAudioThreadForProAudio();

#endif

#if JUCE_IOS
bool urlBookmarkToBinaryData(void * bookmark, const void * & retdata, size_t & retsize);

void *binaryDataToUrlBookmark(const void * data, size_t size);

juce::URL generateUpdatedURL (juce::URL& urlToUse);

#endif
