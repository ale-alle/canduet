# SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
# Copyright (C) 2026 Tin Can Studios
#
# T-091 perf kit (doc/PERF-TEST.md): summarise a CanDuet perf log and say PASS or FAIL.
#
#   powershell -ExecutionPolicy Bypass -File tools\perf-report.ps1 <log.csv>
#   powershell -ExecutionPolicy Bypass -File tools\perf-report.ps1 <log.csv> -Baseline <baseline.csv>
#
# With no log given, the newest log in %APPDATA%\CanDuet-Perf\perf is used.
# Budget (owner 2026-11-26, to be confirmed after the first baseline):
#   0 dropouts, at least 60 minutes, peak audio load under 70%;
#   against a baseline: peak (99th percentile) at most +10 points, average at most +5 points,
#   round-trip latency (median) at most +3 ms (network noise), same sample rate and block size.

param(
    [string]$Log,
    [string]$Baseline,
    [string]$DataFolder = "CanDuet-Perf",
    [double]$MinMinutes = 60,
    [int]$MaxDropouts = 0,
    [double]$MaxPeak = 70,
    [double]$MaxPeakIncrease = 10,
    [double]$MaxAvgIncrease = 5,
    [double]$MaxRttIncreaseMs = 3,
    [double]$SkipSeconds = 15      # app start-up and device opening are not part of the test
)

$ErrorActionPreference = "Stop"

function Read-PerfLog([string]$path) {
    $rows = Get-Content -LiteralPath $path | Where-Object { $_ -and -not $_.StartsWith("#") } | ConvertFrom-Csv
    if (-not $rows) { throw "No data in $path" }
    return @($rows)
}

function Get-Num($v) { if ($null -eq $v -or "$v" -eq "") { return $null }; return [double]::Parse("$v", [Globalization.CultureInfo]::InvariantCulture) }

function Get-Stats([double[]]$values) {
    if (-not $values -or $values.Count -eq 0) { return $null }
    $s = $values | Sort-Object
    $n = $s.Count
    $p = { param($q) $s[[Math]::Min($n - 1, [Math]::Floor($q * ($n - 1) + 0.5))] }
    return [pscustomobject]@{
        Mean   = ($s | Measure-Object -Average).Average
        Median = & $p 0.5
        P99    = & $p 0.99
        Max    = $s[$n - 1]
    }
}

function Get-Summary([string]$path) {
    $all = Read-PerfLog $path
    $rows = @($all | Where-Object { (Get-Num $_.t_s) -ge $SkipSeconds })
    if ($rows.Count -lt 2) { $rows = $all }
    $first = $rows[0]; $last = $rows[$rows.Count - 1]
    $secs = (Get-Num $last.t_s) - (Get-Num $first.t_s)

    $avg  = Get-Stats ([double[]]@($rows | ForEach-Object { 100 * (Get-Num $_.avg_load) }))
    $peak = Get-Stats ([double[]]@($rows | ForEach-Object { 100 * (Get-Num $_.peak_load) }))
    $drop = [int](Get-Num $last.dropouts) - [int](Get-Num $first.dropouts)

    $rttVals = @($rows | ForEach-Object { Get-Num $_.p1_rtt_ms } | Where-Object { $null -ne $_ -and $_ -gt 0 })
    $jitVals = @($rows | ForEach-Object { Get-Num $_.p1_jitter_ms } | Where-Object { $null -ne $_ })
    $bufVals = @($rows | ForEach-Object { Get-Num $_.p1_buffer_ms } | Where-Object { $null -ne $_ })
    $peerRows = @($rows | Where-Object { "$($_.p1_pkts_dropped)" -ne "" })
    $lost = $null; $kin = $null; $kout = $null
    if ($peerRows.Count -ge 2) {
        $a = $peerRows[0]; $b = $peerRows[$peerRows.Count - 1]
        $ps = [Math]::Max(1, (Get-Num $b.t_s) - (Get-Num $a.t_s))
        $lost = (Get-Num $b.p1_pkts_dropped) - (Get-Num $a.p1_pkts_dropped)
        $kin  = 8 * ((Get-Num $b.p1_bytes_in)  - (Get-Num $a.p1_bytes_in))  / $ps / 1000
        $kout = 8 * ((Get-Num $b.p1_bytes_out) - (Get-Num $a.p1_bytes_out)) / $ps / 1000
    }

    return [pscustomobject]@{
        Path = $path; Minutes = $secs / 60
        SampleRate = $last.sample_rate; Block = $last.block
        Avg = $avg; Peak = $peak; Dropouts = $drop
        Rtt = (Get-Stats ([double[]]$rttVals)); Jitter = (Get-Stats ([double[]]$jitVals)); Buffer = (Get-Stats ([double[]]$bufVals))
        PacketsLost = $lost; KbpsIn = $kin; KbpsOut = $kout
        PeerSeconds = $peerRows.Count
    }
}

function Write-Summary($s, [string]$title) {
    Write-Host ""
    Write-Host "== $title" -ForegroundColor Cyan
    Write-Host ("   {0}" -f $s.Path)
    Write-Host ("   Length        {0:N1} min    {1} Hz, block {2}" -f $s.Minutes, $s.SampleRate, $s.Block)
    Write-Host ("   Audio load    average {0:N1}%   peak: median {1:N1}%  99th {2:N1}%  max {3:N1}%" -f $s.Avg.Mean, $s.Peak.Median, $s.Peak.P99, $s.Peak.Max)
    Write-Host ("   Dropouts      {0}" -f $s.Dropouts)
    if ($s.Rtt) {
        Write-Host ("   Round trip    median {0:N1} ms   max {1:N1} ms   (jitter median {2:N1} ms, buffer median {3:N1} ms)" -f $s.Rtt.Median, $s.Rtt.Max, $s.Jitter.Median, $s.Buffer.Median)
        Write-Host ("   Network       packets lost {0}   in {1:N0} kbit/s   out {2:N0} kbit/s   ({3:N1} min connected)" -f $s.PacketsLost, $s.KbpsIn, $s.KbpsOut, ($s.PeerSeconds / 60))
    } else {
        Write-Host "   Network       nobody connected (Practice only)"
    }
}

if (-not $Log) {
    $dir = Join-Path $env:APPDATA (Join-Path $DataFolder "perf")
    $newest = Get-ChildItem -LiteralPath $dir -Filter "perf-*.csv" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $newest) { throw "No log given and none found in $dir" }
    $Log = $newest.FullName
}

$cur = Get-Summary $Log
Write-Summary $cur "This run"

$fails = New-Object System.Collections.Generic.List[string]
$warns = New-Object System.Collections.Generic.List[string]
if ($cur.Minutes -lt $MinMinutes) { $warns.Add(("Only {0:N1} of {1} minutes - run longer for a real result" -f $cur.Minutes, $MinMinutes)) }
if ($cur.Dropouts -gt $MaxDropouts) { $fails.Add("Dropouts: $($cur.Dropouts) (allowed $MaxDropouts)") }
if ($cur.Peak.Max -gt $MaxPeak) { $fails.Add(("Peak audio load {0:N1}% is over {1}%" -f $cur.Peak.Max, $MaxPeak)) }
if (-not $cur.Rtt) { $warns.Add("Nobody was connected, so latency and network weren't tested") }

if ($Baseline) {
    $base = Get-Summary $Baseline
    Write-Summary $base "Baseline"
    if ($base.SampleRate -ne $cur.SampleRate -or $base.Block -ne $cur.Block) {
        $fails.Add("Not comparable: baseline $($base.SampleRate) Hz / block $($base.Block), this run $($cur.SampleRate) Hz / block $($cur.Block). Use the same audio settings.")
    }
    $dp = $cur.Peak.P99 - $base.Peak.P99
    $da = $cur.Avg.Mean - $base.Avg.Mean
    Write-Host ""
    Write-Host "== Change from baseline" -ForegroundColor Cyan
    Write-Host ("   Peak (99th)   {0:+0.0;-0.0} points (allowed +{1})" -f $dp, $MaxPeakIncrease)
    Write-Host ("   Average       {0:+0.0;-0.0} points (allowed +{1})" -f $da, $MaxAvgIncrease)
    if ($dp -gt $MaxPeakIncrease) { $fails.Add(("Peak load up {0:N1} points" -f $dp)) }
    if ($da -gt $MaxAvgIncrease) { $fails.Add(("Average load up {0:N1} points" -f $da)) }
    if ($cur.Rtt -and $base.Rtt) {
        $dr = $cur.Rtt.Median - $base.Rtt.Median
        Write-Host ("   Round trip    {0:+0.0;-0.0} ms (allowed +{1})" -f $dr, $MaxRttIncreaseMs)
        if ($dr -gt $MaxRttIncreaseMs) { $fails.Add(("Round-trip latency up {0:N1} ms" -f $dr)) }
    }
}

Write-Host ""
foreach ($w in $warns) { Write-Host "   note: $w" -ForegroundColor Yellow }
if ($fails.Count -eq 0) {
    Write-Host "RESULT: PASS" -ForegroundColor Green
    exit 0
}
foreach ($f in $fails) { Write-Host "   FAIL: $f" -ForegroundColor Red }
Write-Host "RESULT: FAIL" -ForegroundColor Red
exit 1
