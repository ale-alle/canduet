// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

// MP3 recording through the Windows encoder (T-083). Part of TinCanAudioTests.
// Encodes a tone, reads it back with Windows' decoder, checks length and level.

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <objbase.h>
#endif

#include <JuceHeader.h>
#include "../Source/TinCanMp3Format.h"

#include <cmath>
#include <cstdio>

using namespace juce;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void roundTrip(double rate, int chans)
{
    CHECK(TinCanMp3AudioFormat::canEncode(rate, chans));

    auto file = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("canduet-mp3-test", ".mp3");
    const double seconds = 3.0, freq = 440.0, amp = 0.5;
    const int total = (int) (rate * seconds);
    {
        TinCanMp3AudioFormat fmt;
        auto stream = std::unique_ptr<OutputStream>(file.createOutputStream());
        CHECK(stream != nullptr);
        auto* w = fmt.createWriterFor(stream.get(), rate, (unsigned) chans, 24, {}, 0);
        CHECK(w != nullptr);
        if (w == nullptr) return;
        stream.release();

        // as in the app: made on this thread, written on a recording thread, closed back on this one
        TimeSliceThread thread("Recording Thread");
        thread.startThread();
        auto threaded = std::make_unique<AudioFormatWriter::ThreadedWriter>(w, thread, 32768);

        AudioBuffer<float> buf(chans, 512);
        for (int done = 0; done < total; ) {
            const int n = jmin(512, total - done);
            for (int c = 0; c < chans; ++c)
                for (int i = 0; i < n; ++i)
                    buf.setSample(c, i, (float) (amp * std::sin(2.0 * MathConstants<double>::pi * freq * (done + i) / rate)));
            while (! threaded->write(buf.getArrayOfReadPointers(), n)) Thread::sleep(1);   // FIFO full: let the thread catch up
            done += n;
        }
        threaded.reset();   // writer drains the encoder and closes the file here
        thread.stopThread(1000);
    }

    // about bitrate * seconds bytes
    const double expectBytes = TinCanMp3AudioFormat::bitrateFor(chans) * 1000.0 / 8.0 * seconds;
    CHECK(file.getSize() > expectBytes * 0.9 && file.getSize() < expectBytes * 1.1);

    AudioFormatManager mgr;
    mgr.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r(mgr.createReaderFor(file));
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK((int) r->numChannels == chans);
        CHECK(std::abs(r->sampleRate - rate) < 1.0);
        // MP3 adds a frame or two of encoder delay/padding
        CHECK(std::abs((double) r->lengthInSamples - total) < rate * 0.1);

        AudioBuffer<float> back((int) r->numChannels, (int) rate);
        r->read(&back, 0, (int) rate, (int64) rate, true, true);   // the middle second
        for (int c = 0; c < chans; ++c) {
            const double rms = back.getRMSLevel(c, 0, (int) rate);
            CHECK(std::abs(rms - amp / std::sqrt(2.0)) < 0.05);
        }
        std::printf("  mp3 %g Hz x%d: %lld bytes, %lld frames, rms %.3f\n", rate, chans,
                    (long long) file.getSize(), (long long) r->lengthInSamples, back.getRMSLevel(0, 0, (int) rate));
    }
    r.reset();
    file.deleteFile();
}

int runMp3Tests()
{
    const int before = failures;
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // as on the app's message thread
    roundTrip(48000.0, 2);
    roundTrip(44100.0, 1);
    CHECK(! TinCanMp3AudioFormat::canEncode(96000.0, 2));   // these fall back to FLAC in the app
    CHECK(! TinCanMp3AudioFormat::canEncode(48000.0, 4));
#endif
    std::printf("  mp3 recording: %s\n", failures == before ? "ok" : "FAILED");
    return failures;
}
