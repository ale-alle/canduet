// SPDX-License-Identifier: GPLv3-or-later
// CanDuet: MP3 recording through the MP3 encoder that ships with Windows (Media Foundation).
// Write-only: reading MP3 is already handled by JUCE's Windows Media format.

#pragma once

#include <JuceHeader.h>

class TinCanMp3AudioFormat : public juce::AudioFormat
{
public:
    TinCanMp3AudioFormat();
    ~TinCanMp3AudioFormat() override;

    // True when this PC has the Windows MP3 encoder and it accepts this rate and channel count
    // (the encoder takes 32, 44.1 or 48 kHz, mono or stereo). Callers fall back to FLAC otherwise.
    static bool canEncode (double sampleRate, int numChannels);

    // kbps used for a given channel count
    static int bitrateFor (int numChannels) { return numChannels > 1 ? 192 : 128; }

    juce::Array<int> getPossibleSampleRates() override { return { 32000, 44100, 48000 }; }
    juce::Array<int> getPossibleBitDepths() override { return { 16 }; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream*, bool) override { return nullptr; }

    juce::AudioFormatWriter* createWriterFor (juce::OutputStream* streamToWriteTo,
                                              double sampleRateToUse,
                                              unsigned int numberOfChannels,
                                              int bitsPerSample,
                                              const juce::StringPairArray& metadataValues,
                                              int qualityOptionIndex) override;
    using juce::AudioFormat::createWriterFor;
};
