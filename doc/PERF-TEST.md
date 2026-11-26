# CanDuet performance test (T-091, Phase 0)

Audio quality and latency come before every feature. Each new feature must pass this test before it ships.

**Reference computer:** the owner's i7-6700 desktop running Windows 10, capped at 65% processor speed so it behaves like a weaker student laptop.
**Partner:** a second computer that joins the lesson as the student.

## 1. Build the perf test copy (once per branch)

The perf log is only compiled into test builds (`TINCAN_PERF_LOG=ON`). It uses its own settings folder, `%APPDATA%\CanDuet-Perf`, so it never touches the copy you teach with.

```
cmake -G "Visual Studio 17 2022" -A x64 -B build-perf -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DTINCAN_PERF_LOG=ON -DTINCAN_DATA_FOLDER=CanDuet-Perf -DTINCAN_API_BASE=https://staging.tincanstudios.ca
cmake --build build-perf --config Release --parallel --target SonoBus_Standalone
```

Run `build-perf\SonoBus_artefacts\Release\Standalone\CanDuet.exe`. Every run writes a log to `%APPDATA%\CanDuet-Perf\perf\perf-<date>_<time>.csv`, one line a second. The newest 20 logs are kept.

The second computer can use the normal staging installer. It doesn't need the perf build.

## 2. Cap the processor at 65%

Open PowerShell. First write down the current value; on a desktop it is usually 100:

```
powercfg /query SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX
```

Set the cap:

```
powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX 65
powercfg /setactive SCHEME_CURRENT
```

**Undo it afterwards** with the same two commands and `100` instead of `65`.

## 3. Keep the audio settings the same every time

The audio settings must match between runs, or the results can't be compared. The report refuses to compare runs whose sample rate or block size differ. Record yours here once:

| Setting | Value |
|---|---|
| Audio device / driver | |
| Sample rate | |
| Buffer (block) size | |
| Instrument (VST) on strip 1 | |
| Video on | yes |

## 4. The 60-minute run

Close other heavy programs, such as browsers with many tabs and game launchers. Start the lesson from staging with the second computer as the student. On the second computer, play a song through its file player shared into the lesson, so there is always sound coming in.

| Minutes | What to do on the reference computer |
|---|---|
| 0–5 | Join, video on, talk |
| 5–20 | Play the VST instrument from the MIDI keyboard, metronome on |
| 20–30 | Open Score, play a score back |
| 30–40 | Record the lesson |
| 40–50 | Reverb and delay on, mixer popped out |
| 50–60 | **Everything at once:** VST + metronome + score playback + recording + effects + video |

If you hear a click or dropout, write down the minute. The log shows what happened then.

## 5. Read the result

```
powershell -ExecutionPolicy Bypass -File tools\perf-report.ps1
```

With no file named, the newest log is used. The report shows:
- length, audio load (average and peak), and dropouts
- round-trip latency, jitter and buffer
- packets lost and bandwidth

It ends with **PASS** or **FAIL**.

To compare a feature build with the baseline:

```
powershell -ExecutionPolicy Bypass -File tools\perf-report.ps1 <new.csv> -Baseline <baseline.csv>
```

## 6. The budget

| Rule | Limit |
|---|---|
| Dropouts in 60 minutes | 0 |
| Peak audio load (max) | under 70% |
| Peak load vs baseline (99th percentile) | at most +10 points |
| Average load vs baseline | at most +5 points |
| Round-trip latency vs baseline (median) | at most +3 ms (allowance for internet noise) |

These limits are a proposal (owner 2026-11-26). They get confirmed or adjusted after the first baseline run. Change them in the `param()` block at the top of `tools/perf-report.ps1`.

## 7. Keep the baseline

Copy the passing baseline log somewhere safe, outside the `%APPDATA%` rotation, for example:
`D:\GitHub\canduet-perf\baseline-<branch>-<date>.csv`.
A new baseline is taken only when you decide to, such as after an audio-engine change you've accepted.

## What gets measured, and how

- **avg_load:** the CPU meter's smoothed share of the audio callback's time budget (`AudioProcessLoadMeasurer`, T-087).
- **peak_load:** the slowest single audio block in that second, as a share of that block's time. 1.0 means the block used all its time, so a dropout is likely. This comes from `TinCan::PeakLoad`, which is two clock reads and a lock-free max on the audio thread, and only exists in perf builds.
- **dropouts:** late audio blocks since the device started, from the app's measure or the driver's count, whichever is higher.
- **p1_… p3_:** the first three connected people:
  - ping, round trip (the estimate SonoBus shows), jitter and jitter buffer
  - buffer fill
  - packets dropped and resent
  - bytes in and out
- **Video bandwidth** isn't in the log yet. Video runs in WebView2 and isn't measured here; that's planned when video shedding is built.
