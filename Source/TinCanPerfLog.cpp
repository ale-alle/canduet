// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#include "TinCanPerfLog.h"

#if TINCAN_PERF_LOG

#include "SonobusPluginProcessor.h"
#include "TinCanPaths.h"

namespace TinCan {

String PerfLog::headerLine()
{
    StringArray cols { "t_s", "clock", "sample_rate", "block", "avg_load", "peak_load", "dropouts", "peers" };
    for (int i = 1; i <= maxPeers; ++i)
        for (auto c : { "ping_ms", "rtt_ms", "jitter_ms", "buffer_ms", "fill", "pkts_dropped", "pkts_resent", "bytes_in", "bytes_out" })
            cols.add ("p" + String (i) + "_" + c);
    return cols.joinIntoString (",");
}

PerfLog::PerfLog (SonobusAudioProcessor & p) : processor (p)
{
    openFile();
    startMs = Time::getMillisecondCounter();
    startTimer (1000);
}

PerfLog::~PerfLog()
{
    stopTimer();
    if (out) out->flush();
}

void PerfLog::openFile()
{
    auto dir = getDataFolder().getChildFile ("perf");
    if (! dir.createDirectory()) return;

    // keep the newest (keepFiles - 1) so there's room for this one
    auto old = dir.findChildFiles (File::findFiles, false, "perf-*.csv");
    std::sort (old.begin(), old.end(), [] (const File & a, const File & b) { return a.getFileName() > b.getFileName(); });
    for (int i = keepFiles - 1; i < old.size(); ++i) old.getReference (i).deleteFile();

    auto f = dir.getChildFile ("perf-" + Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S") + ".csv")
                .getNonexistentSibling();
    out = std::make_unique<FileOutputStream> (f);
    if (out->failedToOpen()) { out.reset(); return; }
    out->writeText ("# CanDuet " + String (ProjectInfo::versionString) + " perf log\n", false, false, nullptr);
    out->writeText (headerLine() + "\n", false, false, nullptr);
    out->flush();
}

void PerfLog::timerCallback()
{
    if (! out) return;

    StringArray v;
    v.add (String ((Time::getMillisecondCounter() - startMs) / 1000.0, 1));
    v.add (Time::getCurrentTime().formatted ("%H:%M:%S"));
    v.add (String ((int) processor.getSampleRate()));
    v.add (String (processor.getCurrSamplesPerBlock()));
    v.add (String (processor.getAudioLoad(), 3));
    v.add (String (processor.takePeakAudioLoad(), 3));
    v.add (String (processor.getAudioDropouts()));

    Array<int> connected;
    for (int i = 0; i < processor.getNumberRemotePeers(); ++i)
        if (processor.getRemotePeerConnected (i)) connected.add (i);
    v.add (String (connected.size()));

    for (int slot = 0; slot < maxPeers; ++slot) {
        if (slot >= connected.size()) {
            for (int k = 0; k < 9; ++k) v.add ({});
            continue;
        }
        const int i = connected[slot];
        SonobusAudioProcessor::LatencyInfo lat;
        const bool hasLat = processor.getRemotePeerLatencyInfo (i, lat);
        float fill = 0.0f, fillDev = 0.0f;
        const bool hasFill = processor.getRemotePeerReceiveBufferFillRatio (i, fill, fillDev);
        v.add (hasLat ? String (lat.pingMs, 1) : String());
        v.add (hasLat ? String (lat.totalRoundtripMs, 1) : String());
        v.add (hasLat ? String (lat.jitterMs, 1) : String());
        v.add (String (processor.getRemotePeerBufferTime (i), 1));
        v.add (hasFill ? String (fill, 2) : String());
        v.add (String (processor.getRemotePeerPacketsDropped (i)));
        v.add (String (processor.getRemotePeerPacketsResent (i)));
        v.add (String (processor.getRemotePeerBytesReceived (i)));
        v.add (String (processor.getRemotePeerBytesSent (i)));
    }

    out->writeText (v.joinIntoString (",") + "\n", false, false, nullptr);
    out->flush();   // one short line a second; a crash still leaves a complete log
}

}

#endif
