// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
//
// CanDuet shared score page (T-080): draws the model (score-model.js) with VexFlow, turns clicks,
// keys and MIDI notes into operations, and keeps the copies of everyone in the lesson the same.
//
// App bridge (WebView2 postMessage, JSON):
//   page -> app  {t:"ready"}
//                {t:"send", data}                       to everyone in the lesson (tag canduet-score-1)
//                {t:"play", events:[[beat,len,midi]], end, bpb, click, program}
//                {t:"stop"}
//                {t:"tempo", bpm}                       sets the metronome (shared with everyone)
//   app -> page  {t:"init", name, practice, bpm}
//                {t:"peer", from, data}                 a message from someone in the lesson
//                {t:"peers", names:[...]}               who's in the lesson now
//                {t:"tempo", bpm}                       the metronome's tempo changed
//                {t:"pos", beat, playing, countIn}      playback position, about 30 times a second
//                {t:"notes", e:[[type,ch,note,value,source],...]}  this computer's MIDI input
//
// Peer messages (each under 3000 bytes):
//   {k:"ops", ops:[...]}   new changes, or part of a whole score sent to repair a difference
//   {k:"sum", d}           fingerprint of my copy, every 2 s; a different one makes both send all
"use strict";

(function () {
  const S = window.CanDuetScore;
  const VF = window.Vex && window.Vex.Flow;
  const $ = (sel) => document.querySelector(sel);
  const host = window.chrome && window.chrome.webview;   // the CanDuet app; absent in a plain browser
  const send = (msg) => { if (host) host.postMessage(JSON.stringify(msg)); };

  const params = new URLSearchParams(location.hash.slice(1));
  let myName = (params.get("name") || "Me").slice(0, 80) || "Me";
  let practice = params.get("practice") === "1";

  const state = S.emptyState();
  let session = S.createSession(state, myName);

  // ---------------------------------------------------------------------------
  // entry state
  // ---------------------------------------------------------------------------
  let baseDur = 480, dotted = false, restMode = false, midiWrite = false;
  let cursor = null;     // {m: measure id, t: ticks}
  let sel = null;        // {id: note id, pi: index in its pitches (sorted low..high)}
  let lastWritten = null;  // note id of the last note written here, for Shift+letter chords
  let undoStack = [], redoStack = [];
  let playing = false, playPos = null, countIn = 0;
  let tempo = 100;
  const remoteFresh = new Map();  // note id -> time it arrived from someone else (shown tinted briefly)

  const curDur = () => (dotted ? baseDur * 1.5 : baseDur);

  // ---------------------------------------------------------------------------
  // applying changes (mine and everyone's)
  // ---------------------------------------------------------------------------

  // What an operation would change, as operations that put it back (for undo). Called before the
  // operation is applied.
  function inverseOf(op) {
    switch (op.type) {
      case "setMeta": {
        const back = {};
        for (const k of Object.keys(op.changes)) back[k] = state.meta[k];
        return [session.setMeta(back)];
      }
      case "addMeasure": return [session.removeMeasure(op.id)];
      case "setMeasure": {
        const m = state.measures[op.id];
        if (!m) return [];
        const back = {};
        for (const k of Object.keys(op.changes)) back[k] = m[k];
        return [session.setMeasure(op.id, back)];
      }
      case "removeMeasure": {
        const m = state.measures[op.id];
        if (!m) return [];
        const add = session.addMeasure(m.pos, m.time, m.key);
        const out = [add];
        for (const n of Object.values(state.notes)) {
          if (n.m === m.id) out.push(session.setNote(null, { m: add.id, t: n.t, d: n.d, p: n.p, tie: n.tie }));
        }
        return out;
      }
      case "setNote": {
        const n = state.notes[op.id];
        if (!n) return [session.removeNote(op.id)];
        return [session.setNote(op.id, { m: n.m, t: n.t, d: n.d, p: n.p, tie: n.tie })];
      }
      case "removeNote": {
        const n = state.notes[op.id];
        if (!n) return [];
        return [session.setNote(null, { m: n.m, t: n.t, d: n.d, p: n.p, tie: n.tie })];
      }
      case "reset": {
        const ops = S.opsFromFile(session, S.toFile(state));
        return ops || [];
      }
    }
    return [];
  }

  // One action by this person: apply, remember how to undo it, and send it.
  function act(ops, opts = {}) {
    ops = ops.filter(Boolean);
    if (!ops.length) return;
    const inverse = [];
    for (const op of ops) {
      inverse.unshift(...inverseOf(op));
      S.applyOp(state, op);
    }
    // the inverse was made with timestamps from before the action; give it fresh ones when used
    if (!opts.noUndo) {
      undoStack.push(inverse);
      if (undoStack.length > 200) undoStack.shift();
      if (!opts.keepRedo) redoStack = [];
    }
    queueOut(ops);
    render();
    return inverse;
  }

  // re-stamp operations so they count as written now
  function restamp(ops) {
    return ops.map((op) => { state.clock += 1; return { ...op, ts: state.clock, by: myName }; });
  }

  function undo() {
    const inv = undoStack.pop();
    if (!inv) return;
    const redo = act(restamp(inv), { noUndo: true });
    if (redo) redoStack.push(redo);
    sel = null;
    render();
  }
  function redo() {
    const ops = redoStack.pop();
    if (!ops) return;
    act(restamp(ops), { keepRedo: true });
    sel = null;
    render();
  }

  // ---------------------------------------------------------------------------
  // sharing with the others in the lesson
  // ---------------------------------------------------------------------------
  const MAX_MSG = 2900;
  const enc = new TextEncoder();
  let outQueue = [];
  let peers = [];
  let lastHeard = 0, lastHeardFrom = "";
  let fullSentAt = 0, fullBackoff = 3000;

  function queueOut(ops) {
    if (!host || practice) return;
    outQueue.push(...ops);
  }

  // split ops into messages under the size limit
  function sendOps(ops) {
    let batch = [], size = 20;
    const flush = () => {
      if (batch.length) send({ t: "send", data: JSON.stringify({ k: "ops", ops: batch }) });
      batch = []; size = 20;
    };
    for (const op of ops) {
      const s = enc.encode(JSON.stringify(op)).length + 1;
      if (s > MAX_MSG - 30) continue;  // can't happen with valid ops; never send a broken message
      if (size + s > MAX_MSG) flush();
      batch.push(op); size += s;
    }
    flush();
  }

  setInterval(() => {
    if (outQueue.length) { const ops = outQueue; outQueue = []; sendOps(ops); }
  }, 50);

  function sendFull() {
    const now = Date.now();
    if (now - fullSentAt < fullBackoff) return;
    fullSentAt = now;
    fullBackoff = Math.min(fullBackoff * 2, 60000);  // if copies still differ, back off
    const ops = S.snapshotOps(state);
    // newest tombstones only, so a long lesson doesn't resend thousands of removals
    const removes = ops.filter((o) => o.type === "removeNote" || o.type === "removeMeasure").sort((a, b) => b.ts - a.ts).slice(0, 1500);
    sendOps(ops.filter((o) => o.type !== "removeNote" && o.type !== "removeMeasure").concat(removes));
  }

  setInterval(() => {
    if (!host || practice || peers.length === 0) return;
    send({ t: "send", data: JSON.stringify({ k: "sum", d: S.digest(state) }) });
  }, 2000);

  function receivePeer(from, data) {
    let msg;
    try { msg = JSON.parse(data); } catch (e) { return; }
    if (!msg || typeof msg !== "object") return;
    lastHeard = Date.now();
    lastHeardFrom = String(from || "").slice(0, 80);
    if (msg.k === "ops" && Array.isArray(msg.ops) && msg.ops.length <= 400) {
      let changed = false;
      for (const op of msg.ops) {
        const known = op && typeof op === "object" && state.notes[op.id];
        if (S.applyOp(state, op)) {
          changed = true;
          if (op.type === "setNote" && !known) remoteFresh.set(op.id, Date.now());
        }
      }
      if (changed) { fixSelection(); render(); }
    }
    else if (msg.k === "sum" && typeof msg.d === "string") {
      if (msg.d === S.digest(state)) { fullBackoff = 3000; }
      else sendFull();
    }
    updateStatus();
  }

  // ---------------------------------------------------------------------------
  // an empty score starts with four bars of 4/4; both sides make exactly the same ones, so two
  // people opening the score at once don't get eight
  // ---------------------------------------------------------------------------
  function ensureStart() {
    if (state.resetTs > 0 || Object.keys(state.measures).length > 0) return;
    let pos = "";
    for (let i = 1; i <= 4; i++) {
      pos = S.posBetween(pos, "");
      S.applyOp(state, { v: S.VERSION, type: "addMeasure", id: "m.start." + i, by: "start", ts: 1, pos, time: i === 1 ? [4, 4] : null, key: i === 1 ? 0 : null });
    }
  }

  // ---------------------------------------------------------------------------
  // drawing
  // ---------------------------------------------------------------------------
  const DUR_CODE = { 1920: "w", 960: "h", 480: "q", 240: "8", 120: "16", 60: "32" };
  const LETTERS = "cdefgab";
  const KEY_NAMES = { "-7": "Cb", "-6": "Gb", "-5": "Db", "-4": "Ab", "-3": "Eb", "-2": "Bb", "-1": "F", 0: "C",
                      1: "G", 2: "D", 3: "A", 4: "E", 5: "B", 6: "F#", 7: "C#" };
  const KEY_LABELS = { "-7": "C♭ major", "-6": "G♭ major", "-5": "D♭ major", "-4": "A♭ major", "-3": "E♭ major",
                       "-2": "B♭ major", "-1": "F major", 0: "C major / A minor", 1: "G major", 2: "D major", 3: "A major",
                       4: "E major", 5: "B major", 6: "F♯ major", 7: "C♯ major" };
  const TIMES = [[2, 4], [3, 4], [4, 4], [5, 4], [6, 4], [2, 2], [3, 2], [3, 8], [6, 8], [9, 8], [12, 8], [5, 8], [7, 8]];
  // a row: VexFlow puts the top staff line 4 spaces below the stave's y, so the staff sits at
  // 50-90 px with room for ledger lines (and chord names later) above and below
  const ROW_H = 150, STAVE_TOP = 10;

  let layout = [];   // per measure: {id, m (view), x, w, y, stave, ticksX:[[tick,x]], notes:[{item, sn}]}
  let lastView = null;
  let renderQueued = false;

  function durCode(d) {
    if (DUR_CODE[d]) return { code: DUR_CODE[d], dots: 0 };
    if (DUR_CODE[d / 1.5]) return { code: DUR_CODE[d / 1.5], dots: 1 };
    return { code: "q", dots: 0 };
  }
  function vfKey(p) {
    const acc = p[2] === 1 ? "#" : p[2] === 2 ? "##" : p[2] === -1 ? "b" : p[2] === -2 ? "bb" : "";
    return `${LETTERS[p[0]]}${acc}/${p[1]}`;
  }
  const clefBase = () => (state.meta.clef === "bass" ? 16 : 28);  // diatonic index on VexFlow line 0
  const diatonic = (p) => p[1] * 7 + p[0];

  function render() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; draw(); });
  }

  function draw() {
    const v = S.view(state);
    lastView = v;
    const scoreEl = $("#score");
    const overlay = $("#overlay");
    for (const el of Array.from(scoreEl.children)) if (el !== overlay) el.remove();
    $("#empty").hidden = v.measures.length > 0;
    if (!cursorValid(v)) cursor = v.measures.length ? { m: v.measures[0].id, t: 0 } : null;
    syncControls(v);
    layout = [];
    if (!VF || v.measures.length === 0) { drawOverlay(); return; }

    const width = Math.max(320, scoreEl.clientWidth || 800);
    const clef = state.meta.clef;
    const now = Date.now();

    // build each measure's notes first, to know how wide it wants to be
    const built = v.measures.map((m) => buildMeasure(m, clef, now));

    // rows: as many measures as fit; the first of each row repeats clef and key
    const rows = [];
    let row = [], used = 0;
    for (let i = 0; i < built.length; i++) {
      const b = built[i];
      const w = b.minW + (row.length === 0 ? b.headW(true) : b.headW(false));
      if (row.length && used + w > width) { rows.push(row); row = []; used = 0; }
      const w2 = b.minW + (row.length === 0 ? b.headW(true) : b.headW(false));
      row.push({ b, w: w2 });
      used += w2;
    }
    if (row.length) rows.push(row);

    const renderer = new VF.Renderer(document.createElement("div"), VF.Renderer.Backends.SVG);
    renderer.resize(width, rows.length * ROW_H + 20);
    const ctx = renderer.getContext();
    ctx.setFont("Segoe UI", 10);

    const tieQueue = [];
    rows.forEach((r, ri) => {
      const total = r.reduce((s, x) => s + x.w, 0);
      const last = ri === rows.length - 1;
      const stretch = last && total < width * 0.75 ? Math.min(1.3, width / total) : width / total;
      let x = 0;
      r.forEach((cell, ci) => {
        const w = Math.floor(cell.w * stretch) - (ci === r.length - 1 && stretch !== 1 ? 1 : 0);
        const geo = drawMeasure(ctx, cell.b, x, ri * ROW_H + STAVE_TOP, w, ci === 0, clef, tieQueue);
        layout.push({ ...geo, row: ri });
        x += w;
      });
    });

    // ties: to the next note with the same pitch, which may be in the next measure
    for (const t of tieQueue) {
      try {
        const sameRow = t.to && t.to.row === t.from.row;
        if (t.to && sameRow) {
          new VF.StaveTie({ first_note: t.from.sn, last_note: t.to.sn, first_indices: t.fi, last_indices: t.li }).setContext(ctx).draw();
        } else {
          new VF.StaveTie({ first_note: t.from.sn, last_note: null, first_indices: t.fi, last_indices: t.fi }).setContext(ctx).draw();
          if (t.to) new VF.StaveTie({ first_note: null, last_note: t.to.sn, first_indices: t.li, last_indices: t.li }).setContext(ctx).draw();
        }
      } catch (e) { /* a tie that can't be drawn is skipped */ }
    }

    const svg = ctx.svg;
    svg.setAttribute("aria-hidden", "true");
    scoreEl.insertBefore(svg, overlay);
    drawOverlay();
  }

  function buildMeasure(m, clef, now) {
    const keyName = KEY_NAMES[m.key];
    const notes = [];
    for (const it of m.items) {
      let sn;
      if (it.rest) {
        const { code, dots } = it.whole ? { code: "w", dots: 0 } : durCode(it.d);
        // a whole rest hangs from the fourth line, the others sit in the middle
        const restKey = code === "w" ? (clef === "bass" ? "f/3" : "d/5") : (clef === "bass" ? "d/3" : "b/4");
        sn = new VF.StaveNote({ clef, keys: [restKey], duration: code + (dots ? "d" : "") + "r", align_center: !!it.whole });
        if (dots) VF.Dot.buildAndAttach([sn], { all: true });
      } else {
        const { code, dots } = durCode(it.d);
        const pitches = it.p.slice().sort((a, b) => S.midiOf(a) - S.midiOf(b));
        sn = new VF.StaveNote({ clef, keys: pitches.map(vfKey), duration: code + (dots ? "d" : ""), auto_stem: true });
        if (dots) VF.Dot.buildAndAttach([sn], { all: true });
        const selected = sel && sel.id === it.id;
        const fresh = remoteFresh.has(it.id) && now - remoteFresh.get(it.id) < 4000;
        if (selected) sn.setStyle({ fillStyle: "#e5484d", strokeStyle: "#e5484d" });
        else if (fresh) sn.setStyle({ fillStyle: "#1d7fd0", strokeStyle: "#1d7fd0" });
        it.sorted = pitches;
      }
      notes.push({ item: it, sn });
    }
    const voice = new VF.Voice({ num_beats: m.time[0], beat_value: m.time[1] }).setMode(VF.Voice.Mode.SOFT);
    voice.addTickables(notes.map((n) => n.sn));
    try { VF.Accidental.applyAccidentals([voice], keyName); } catch (e) { /* drawn without */ }
    let beams = [];
    try {
      // rests included so beams stop at them
      beams = VF.Beam.generateBeams(notes.map((n) => n.sn), { groups: VF.Beam.getDefaultBeamGroups(m.time.join("/")), beam_rests: false });
    } catch (e) { beams = []; }
    const fmt = new VF.Formatter();
    fmt.joinVoices([voice]);
    let minW = 0;
    try { minW = fmt.preCalculateMinTotalWidth([voice]); } catch (e) { minW = notes.length * 28; }
    minW = Math.max(150, minW * 1.7 + 50);
    const headW = (first) => {
      let w = 0;
      if (first) w += 34 + Math.abs(m.key) * 10;
      else if (m.keyChanged) w += Math.abs(m.key) * 10 + 14;
      if (m.timeChanged) w += 30;
      return w;
    };
    return { m, voice, notes, beams, fmt, minW, headW };
  }

  function drawMeasure(ctx, b, x, y, w, first, clef, tieQueue) {
    const m = b.m;
    const stave = new VF.Stave(x, y, w);
    if (first) { stave.addClef(clef); if (m.key !== 0 || m.keyChanged) stave.addKeySignature(KEY_NAMES[m.key]); }
    else if (m.keyChanged) stave.addKeySignature(KEY_NAMES[m.key]);
    if (m.timeChanged) stave.addTimeSignature(m.time.join("/"));
    if (first) stave.setMeasure(m.index + 1);
    if (m.index === (lastView.measures.length - 1)) stave.setEndBarType(VF.Barline.type.END);
    stave.setContext(ctx).draw();
    const startX = stave.getNoteStartX(), endX = stave.getNoteEndX();
    try { b.fmt.format([b.voice], Math.max(20, endX - startX - 14)); } catch (e) { /* leave unformatted */ }
    try { b.voice.draw(ctx, stave); } catch (e) { /* skip */ }
    for (const beam of b.beams) { try { beam.setContext(ctx).draw(); } catch (e) { /* skip */ } }

    const ticksX = [];
    const geoNotes = [];
    for (const n of b.notes) {
      let nx;
      try { nx = n.sn.getNoteHeadBeginX ? n.sn.getNoteHeadBeginX() : n.sn.getAbsoluteX(); } catch (e) { nx = startX; }
      if (n.item.whole) nx = startX + 6;
      ticksX.push([n.item.t, nx]);
      const ys = [];
      try { if (!n.item.rest) for (const yy of n.sn.getYs()) ys.push(yy); } catch (e) { /* none */ }
      geoNotes.push({ item: n.item, sn: n.sn, x: nx, ys, row: null });
    }
    ticksX.push([m.len, endX - 4]);
    const geo = { id: m.id, m, x, w, y, stave, startX, endX, ticksX, notes: geoNotes };
    for (const g of geoNotes) g.row = Math.round(y / ROW_H);

    // ties start here; the end is found when the next measure is drawn
    for (const g of geoNotes) {
      if (g.item.rest || !g.item.tie) continue;
      const to = findTieTarget(m, g.item);
      tieQueue.push({ from: g, to: null, fi: [], li: [], target: to });
    }
    // resolve ties whose targets are in this measure (or earlier ones in this one)
    for (const t of tieQueue) {
      if (t.to || !t.target) continue;
      const tg = [...layout.flatMap((l) => l.notes), ...geoNotes].find((g) => g.item.id === t.target.id);
      if (!tg) continue;
      const fi = [], li = [];
      t.from.item.sorted.forEach((p, i) => {
        const j = tg.item.sorted.findIndex((q) => S.midiOf(q) === S.midiOf(p));
        if (j >= 0) { fi.push(i); li.push(j); }
      });
      if (fi.length) { t.to = tg; t.fi = fi; t.li = li; }
      else t.target = null;
    }
    return geo;
  }

  // the note right after this one (same measure, or the first in the next)
  function findTieTarget(m, item) {
    const ms = lastView.measures;
    const idx = ms.findIndex((x) => x.id === m.id);
    const inThis = m.items.find((i) => !i.rest && i.t === item.t + item.d);
    if (inThis) return inThis;
    if (item.t + item.d >= m.len && ms[idx + 1]) {
      const first = ms[idx + 1].items[0];
      if (first && !first.rest) return first;
    }
    return null;
  }

  function xForTick(geo, tick) {
    const pts = geo.ticksX;
    for (let i = 0; i < pts.length - 1; i++) {
      const [t0, x0] = pts[i], [t1, x1] = pts[i + 1];
      if (tick >= t0 && tick <= t1) return t1 === t0 ? x0 : x0 + ((x1 - x0) * (tick - t0)) / (t1 - t0);
    }
    return pts.length ? pts[pts.length - 1][1] : geo.startX;
  }

  function cursorValid(v) {
    if (!cursor) return false;
    const m = v.measures.find((x) => x.id === cursor.m);
    return !!m && cursor.t < m.len;
  }

  function drawOverlay(ghost) {
    const overlay = $("#overlay");
    overlay.textContent = "";
    const add = (cls, style) => {
      const d = document.createElement("div");
      d.className = cls;
      Object.assign(d.style, style);
      overlay.appendChild(d);
      return d;
    };
    if (cursor) {
      const g = layout.find((l) => l.id === cursor.m);
      if (g) {
        const top = g.stave.getYForLine(0), bottom = g.stave.getYForLine(4);
        add("cursor", { left: xForTick(g, cursor.t) - 4 + "px", top: top - 10 + "px", height: bottom - top + 20 + "px" });
        add("barsel", { left: g.x + 1 + "px", top: top - 14 + "px", width: g.w - 2 + "px", height: bottom - top + 28 + "px" });
      }
    }
    if (ghost) {
      const g = ghost.geo;
      const x = xForTick(g, ghost.t) + 6;
      if (ghost.rest) {
        add("ghost rest", { left: x + "px", top: g.stave.getYForLine(2) + "px" });
      } else {
        const line = (diatonic(ghost.p) - clefBase()) / 2;
        const y = g.stave.getYForNote(line);
        add("ghost", { left: x + "px", top: y + "px" });
        // ledger lines above or below the staff
        for (let l = 0; l >= line; l--) if (Number.isInteger(l)) add("ledger", { left: x + "px", top: g.stave.getYForNote(l) + "px" });
        for (let l = 6; l <= line; l++) add("ledger", { left: x + "px", top: g.stave.getYForNote(l) + "px" });
      }
    }
    if (playing && playPos !== null && playPos >= 0 && lastView) {
      const mv = lastView.measures.find((m) => playPos >= m.beat && playPos < m.beat + m.len / S.PPQ);
      const g = mv && layout.find((l) => l.id === mv.id);
      if (g) {
        const top = g.stave.getYForLine(0), bottom = g.stave.getYForLine(4);
        const x = xForTick(g, (playPos - mv.beat) * S.PPQ);
        add("playhead", { left: x + "px", top: top - 18 + "px", height: bottom - top + 36 + "px" });
        followPlayhead(top);
      }
    }
  }

  let lastScrollRow = -1;
  function followPlayhead(top) {
    const sheet = $("#sheet");
    const scoreTop = $("#score").offsetTop + $("#paper").offsetTop;
    const yIn = scoreTop + top;
    const row = Math.floor(top / ROW_H);
    if (row !== lastScrollRow && (yIn < sheet.scrollTop + 40 || yIn > sheet.scrollTop + sheet.clientHeight - ROW_H)) {
      sheet.scrollTo({ top: Math.max(0, yIn - 60), behavior: "smooth" });
    }
    lastScrollRow = row;
  }

  // ---------------------------------------------------------------------------
  // hit testing
  // ---------------------------------------------------------------------------
  function hit(ev) {
    const r = $("#score").getBoundingClientRect();
    const x = ev.clientX - r.left, y = ev.clientY - r.top;
    const geo = layout.find((g) => x >= g.x && x < g.x + g.w && y >= g.y - STAVE_TOP && y < g.y - STAVE_TOP + ROW_H);
    if (!geo) return null;
    // pitch from the height: half a staff space per step
    const sp = geo.stave.getSpacingBetweenLines();
    let line = (geo.stave.getYForNote(0) - y) / sp;
    line = Math.max(-5, Math.min(11, Math.round(line * 2) / 2));
    const d = clefBase() + Math.round(line * 2);
    const step = ((d % 7) + 7) % 7, oct = Math.floor(d / 7);
    const p = [step, oct, S.keyAlter(geo.m.key, step)];
    // an existing note near the pointer?
    let near = null, best = 12;
    for (const g of geo.notes) {
      if (g.item.rest) continue;
      const dx = Math.abs(x - (g.x + 6));
      if (dx < best) {
        // which notehead (closest in height)
        let pi = 0, dy = Infinity;
        g.ys.forEach((yy, i) => { if (Math.abs(yy - y) < dy) { dy = Math.abs(yy - y); pi = i; } });
        if (dy < sp * 0.9) { near = { g, pi }; best = dx; }
      }
    }
    // the grid spot nearest the pointer, in steps of the note length being written
    const step0 = Math.min(baseDur, 960);
    let tick = 0, bx = Infinity;
    for (let t = 0; t < geo.m.len; t += step0) {
      const dx = Math.abs(xForTick(geo, t) + 6 - x);
      if (dx < bx) { bx = dx; tick = t; }
    }
    // also allow the start of any existing note or rest (so a dotted rhythm can be clicked into)
    for (const g of geo.notes) {
      const dx = Math.abs(g.x + 6 - x);
      if (dx < bx) { bx = dx; tick = g.item.t; }
    }
    return { geo, tick, p, near, line };
  }

  // ---------------------------------------------------------------------------
  // writing
  // ---------------------------------------------------------------------------
  function measureOf(id) { return lastView && lastView.measures.find((m) => m.id === id); }

  // the longest allowed length that fits in `room`, no longer than `want`
  function fitDur(want, room) {
    if (want <= room) return want;
    return S.DURS.filter((d) => d <= room).sort((a, b) => b - a)[0] || 0;
  }

  // operations that clear [t, t+d) in a measure: notes inside go, notes reaching in are shortened
  function clearOps(m, t, d) {
    const ops = [];
    for (const n of Object.values(state.notes)) {
      if (n.m !== m.id) continue;
      const end = n.t + n.d;
      if (end <= t || n.t >= t + d) continue;
      if (n.t < t && S.DURS.includes(t - n.t)) ops.push(session.setNote(n.id, { m: n.m, t: n.t, d: t - n.t, p: n.p, tie: false }));
      else ops.push(session.removeNote(n.id));
    }
    return ops;
  }

  // next spot after [m, t+d); adds a bar at the end when needed
  function advance(m, t, d, extraOps) {
    if (t + d < m.len) { cursor = { m: m.id, t: t + d }; return; }
    const ms = lastView.measures;
    const next = ms[m.index + 1];
    if (next) { cursor = { m: next.id, t: 0 }; return; }
    const add = session.addMeasure(S.posBetween(state.measures[m.id].pos, ""), null, null);
    extraOps.push(add);
    cursor = { m: add.id, t: 0 };
  }

  function writeAt(mId, t, pitches, asRest) {
    const m = measureOf(mId);
    if (!m) return;
    const d = fitDur(curDur(), m.len - t);
    if (!d) return;
    const ops = clearOps(m, t, d);
    let id = null;
    if (!asRest) {
      const uniq = [];
      for (const p of pitches) if (!uniq.some((q) => S.midiOf(q) === S.midiOf(p))) uniq.push(p);
      const op = session.setNote(null, { m: m.id, t, d, p: uniq.slice(0, S.LIMITS.pitches), tie: false });
      if (S.validateOp(op) !== null) return;
      ops.push(op);
      id = op.id;
    }
    advance(m, t, d, ops);
    act(ops);
    lastWritten = id;
    // the note just written stays selected, so ↑ ↓ and accidentals fix it straight away; note
    // lengths chosen now are for the next note (entry), not this one
    sel = id ? { id, pi: 0, entry: true } : null;
    render();
  }

  // add a pitch to the chord of note `id`
  function addToChord(id, p) {
    const n = state.notes[id];
    if (!n || n.p.some((q) => S.midiOf(q) === S.midiOf(p)) || n.p.length >= S.LIMITS.pitches) return;
    act([session.setNote(id, { m: n.m, t: n.t, d: n.d, p: n.p.concat([p]), tie: n.tie })]);
    sel = { id, pi: 0 };
  }

  function selectedNote() { return sel && state.notes[sel.id]; }

  // sorted pitches of a note, and the index of the one selected
  function sortedPitches(n) { return n.p.slice().sort((a, b) => S.midiOf(a) - S.midiOf(b)); }

  function changeSelected(fn) {
    const n = selectedNote();
    if (!n) return false;
    const ps = sortedPitches(n);
    const i = Math.min(sel.pi, ps.length - 1);
    const np = fn(ps[i], n);
    if (!np) return true;
    const next = ps.slice();
    next[i] = np;
    if (new Set(next.map(S.midiOf)).size !== next.length) return true;
    const op = session.setNote(n.id, { m: n.m, t: n.t, d: n.d, p: next, tie: n.tie });
    if (S.validateOp(op) === null) act([op]);
    sel = { id: n.id, pi: sortedPitches(state.notes[n.id]).findIndex((q) => S.midiOf(q) === S.midiOf(np)), entry: !!sel.entry };
    render();
    return true;
  }

  function transposeSelected(semis) {
    const m = selectedNote() && measureOf(selectedNote().m);
    return changeSelected((p) => {
      const midi = S.midiOf(p) + semis;
      if (midi < 12 || midi > 115) return null;
      return S.spell(midi, m ? m.key : 0);
    });
  }

  function setAlterSelected(alt) {
    return changeSelected((p) => {
      const np = [p[0], p[1], alt];
      const midi = S.midiOf(np);
      return midi >= 0 && midi <= 127 ? np : null;
    });
  }

  function setDurationSelected() {
    const n = selectedNote();
    if (!n || sel.entry) return false;
    const m = measureOf(n.m);
    if (!m) return false;
    const d = fitDur(curDur(), m.len - n.t);
    if (!d || d === n.d) return true;
    const ops = clearOps(m, n.t + Math.min(d, n.d), Math.max(0, d - n.d)).filter((o) => o.id !== n.id);
    ops.push(session.setNote(n.id, { m: n.m, t: n.t, d, p: n.p, tie: n.tie }));
    act(ops);
    return true;
  }

  function deleteSelected() {
    const n = selectedNote();
    if (!n) return;
    const ps = sortedPitches(n);
    if (ps.length > 1) {
      const rest = ps.filter((_, i) => i !== Math.min(sel.pi, ps.length - 1));
      act([session.setNote(n.id, { m: n.m, t: n.t, d: n.d, p: rest, tie: n.tie })]);
      sel = { id: n.id, pi: 0 };
    } else {
      act([session.removeNote(n.id)]);
      cursor = { m: n.m, t: n.t };
      sel = null;
    }
  }

  function moveSelection(dir) {
    if (!lastView) return;
    const all = [];
    for (const m of lastView.measures) for (const it of m.items) if (!it.rest) all.push({ m, it });
    if (sel) {
      const i = all.findIndex((x) => x.it.id === sel.id);
      const j = i + dir;
      if (i >= 0 && j >= 0 && j < all.length) { sel = { id: all[j].it.id, pi: 0 }; cursor = { m: all[j].m.id, t: all[j].it.t }; }
    } else if (cursor) {
      // move the cursor by the note length being written
      const m = measureOf(cursor.m);
      if (!m) return;
      let t = cursor.t + dir * Math.min(curDur(), m.len);
      let mi = m.index;
      if (t >= m.len) { if (lastView.measures[mi + 1]) { mi++; t = 0; } else t = cursor.t; }
      if (t < 0) { if (mi > 0) { mi--; const pm = lastView.measures[mi]; t = Math.max(0, pm.len - Math.min(curDur(), pm.len)); } else t = 0; }
      cursor = { m: lastView.measures[mi].id, t };
    }
    render();
  }

  // octave for a letter: the one closest to the note before the cursor
  function pitchForLetter(letter, m) {
    const step = LETTERS.indexOf(letter);
    const ref = (lastWritten && state.notes[lastWritten]) ? sortedPitches(state.notes[lastWritten])[0] : null;
    // nothing written yet: around the bottom of the staff (C4..B4 in treble, C3..B3 in bass)
    const refD = ref ? diatonic(ref) : clefBase() + 3;
    let best = null;
    for (let oct = 0; oct <= 8; oct++) {
      const d = oct * 7 + step;
      if (!best || Math.abs(d - refD) < Math.abs(diatonic(best) - refD)) best = [step, oct, S.keyAlter(m.key, step)];
    }
    return best;
  }

  // ---------------------------------------------------------------------------
  // MIDI keyboard: notes played within 60 ms are one chord
  // ---------------------------------------------------------------------------
  let chordBuf = [], chordTimer = null;
  function midiNote(note) {
    if (!midiWrite || !cursor) return;
    chordBuf.push(note);
    if (chordTimer) return;
    chordTimer = setTimeout(() => {
      chordTimer = null;
      const notes = chordBuf; chordBuf = [];
      const m = measureOf(cursor.m);
      if (!m) return;
      const shift = state.meta.instrument === "piano" ? 0 : 12;  // guitar and bass are written an octave up
      const ps = notes.map((n) => S.spell(n + shift, m.key)).filter((p) => S.validPitch(p));
      if (ps.length) writeAt(cursor.m, cursor.t, ps, restMode);
    }, 60);
  }

  // ---------------------------------------------------------------------------
  // playing
  // ---------------------------------------------------------------------------
  const PROGRAMS = { piano: 0, guitar: 25, bass: 33 };
  function startPlay() {
    if (!host || !lastView || !lastView.measures.length) return;
    const from = (cursor && measureOf(cursor.m)) || lastView.measures[0];
    const shift = state.meta.instrument === "piano" ? 0 : -12;
    const evs = S.playEvents(lastView)
      .filter((e) => e.beat >= from.beat - 1e-9)
      .map((e) => [+(e.beat - from.beat).toFixed(6), +e.len.toFixed(6), e.midi + shift])
      .filter((e) => e[2] >= 0 && e[2] <= 127);
    const quarters = (from.time[0] * 4) / from.time[1];
    send({
      t: "play", events: evs.slice(0, 20000), end: lastView.totalBeats - from.beat,
      bpb: Number.isInteger(quarters) ? quarters : 0, click: $("#click").getAttribute("aria-pressed") === "true",
      program: PROGRAMS[state.meta.instrument] || 0,
    });
    playFrom = from.beat;
    playing = true;
    playPos = null;
    syncPlayButton();
  }
  let playFrom = 0;
  function stopPlay() {
    send({ t: "stop" });
    playing = false;
    playPos = null;
    syncPlayButton();
    drawOverlay();
  }
  function syncPlayButton() {
    const b = $("#play");
    b.setAttribute("aria-pressed", String(playing));
    b.textContent = playing ? "■ Stop" : "▶ Play";
  }

  // ---------------------------------------------------------------------------
  // controls
  // ---------------------------------------------------------------------------
  function fillSelects() {
    const time = $("#time");
    for (const t of TIMES) time.add(new Option(t.join("/"), t.join("/")));
    const key = $("#key");
    for (let k = -7; k <= 7; k++) key.add(new Option(KEY_LABELS[k], String(k)));
  }

  function syncControls(v) {
    document.querySelectorAll("[data-dur]").forEach((b) => b.setAttribute("aria-pressed", String(+b.dataset.dur === baseDur)));
    $("#dot").setAttribute("aria-pressed", String(dotted));
    $("#rest").setAttribute("aria-pressed", String(restMode));
    $("#midi").setAttribute("aria-pressed", String(midiWrite));
    $("#clef").value = state.meta.clef;
    $("#instrument").value = state.meta.instrument;
    if (document.activeElement !== $("#title")) $("#title").value = state.meta.title;
    const m = cursor && v.measures.find((x) => x.id === cursor.m);
    $("#time").value = m ? m.time.join("/") : "4/4";
    if (m && !TIMES.some((t) => t.join("/") === m.time.join("/"))) {
      $("#time").add(new Option(m.time.join("/"), m.time.join("/")));
      $("#time").value = m.time.join("/");
    }
    $("#key").value = String(m ? m.key : 0);
    $("#undo").disabled = undoStack.length === 0;
    $("#redo").disabled = redoStack.length === 0;
    $("#del").disabled = !selectedNote();
    $("#tie").disabled = !selectedNote();
    $("#delBar").disabled = !m;
    document.querySelectorAll("[data-alt]").forEach((b) => (b.disabled = !selectedNote()));
  }

  function updateStatus() {
    let s = "";
    if (practice || !host) s = "Practice: just you";
    else if (Date.now() - lastHeard < 6000 && lastHeardFrom) s = "Shared with " + lastHeardFrom;
    else if (peers.length) s = "Shared with " + peers.join(", ");
    else s = "Waiting for the others";
    if (playing && countIn > 0) s = "Count-in…";
    $("#status").textContent = s;
  }
  setInterval(updateStatus, 2000);

  function fixSelection() {
    if (sel && !state.notes[sel.id]) sel = null;
  }

  function bind() {
    document.querySelectorAll("[data-dur]").forEach((b) => b.addEventListener("click", () => {
      baseDur = +b.dataset.dur;
      if (!setDurationSelected()) render(); else render();
    }));
    $("#dot").addEventListener("click", () => { dotted = !dotted; setDurationSelected(); render(); });
    $("#rest").addEventListener("click", () => { restMode = !restMode; render(); });
    $("#tie").addEventListener("click", toggleTie);
    document.querySelectorAll("[data-alt]").forEach((b) => b.addEventListener("click", () => setAlterSelected(+b.dataset.alt)));
    $("#del").addEventListener("click", deleteSelected);
    $("#undo").addEventListener("click", undo);
    $("#redo").addEventListener("click", redo);
    $("#clef").addEventListener("change", (e) => act([session.setMeta({ clef: e.target.value })]));
    $("#instrument").addEventListener("change", (e) => {
      const inst = e.target.value;
      const changes = { instrument: inst };
      if (inst === "bass") changes.clef = "bass";
      if (inst === "guitar") changes.clef = "treble";
      act([session.setMeta(changes)]);
    });
    $("#time").addEventListener("change", (e) => {
      if (!cursor) return;
      const [n, d] = e.target.value.split("/").map(Number);
      act([session.setMeasure(cursor.m, { time: [n, d] })]);
    });
    $("#key").addEventListener("change", (e) => {
      if (!cursor) return;
      act([session.setMeasure(cursor.m, { key: +e.target.value })]);
    });
    const addBar = () => {
      const ms = lastView ? lastView.measures : [];
      const at = cursor ? ms.findIndex((m) => m.id === cursor.m) : ms.length - 1;
      const a = at >= 0 ? state.measures[ms[at].id].pos : "";
      const b = at + 1 < ms.length ? state.measures[ms[at + 1].id].pos : "";
      const op = session.addMeasure(S.posBetween(a, b), ms.length ? null : [4, 4], ms.length ? null : 0);
      act([op]);
      cursor = { m: op.id, t: 0 };
      render();
    };
    $("#addBar").addEventListener("click", addBar);
    $("#emptyAdd").addEventListener("click", addBar);
    $("#delBar").addEventListener("click", () => {
      if (!cursor) return;
      const ms = lastView.measures;
      const i = ms.findIndex((m) => m.id === cursor.m);
      const m = state.measures[cursor.m];
      // the bar after it keeps the time and key it had
      const ops = [];
      const next = ms[i + 1];
      if (next && m) {
        const ch = {};
        if (state.measures[next.id].time === null) ch.time = next.time;
        if (state.measures[next.id].key === null) ch.key = next.key;
        if (Object.keys(ch).length) ops.push(session.setMeasure(next.id, ch));
      }
      ops.push(session.removeMeasure(cursor.m));
      act(ops);
      const nb = ms[i + 1] || ms[i - 1];
      cursor = nb ? { m: nb.id, t: 0 } : null;
      sel = null;
      render();
    });
    $("#midi").addEventListener("click", () => { midiWrite = !midiWrite; render(); });
    $("#title").addEventListener("change", (e) => act([session.setMeta({ title: e.target.value.slice(0, 120) })]));
    $("#title").addEventListener("keydown", (e) => { if (e.key === "Enter") e.target.blur(); e.stopPropagation(); });
    $("#tempo").addEventListener("change", (e) => {
      const bpm = Math.max(20, Math.min(300, Math.round(+e.target.value || tempo)));
      e.target.value = bpm;
      tempo = bpm;
      send({ t: "tempo", bpm });
    });
    $("#tempo").addEventListener("keydown", (e) => e.stopPropagation());
    $("#click").addEventListener("click", (e) => {
      const b = e.currentTarget;
      b.setAttribute("aria-pressed", String(b.getAttribute("aria-pressed") !== "true"));
    });
    $("#play").addEventListener("click", () => (playing ? stopPlay() : startPlay()));
    $("#helpBtn").addEventListener("click", () => {
      const open = $("#help").hidden;
      $("#help").hidden = !open;
      $("#helpBtn").setAttribute("aria-expanded", String(open));
    });
    $("#new").addEventListener("click", () => {
      if (!confirm("Start a new score? This clears it for everyone. (Undo brings it back.)")) return;
      const ops = [session.reset()];
      let pos = "";
      for (let i = 0; i < 4; i++) {
        pos = S.posBetween(pos, "");
        ops.push(session.addMeasure(pos, i === 0 ? [4, 4] : null, i === 0 ? 0 : null));
      }
      act(ops);
      cursor = null; sel = null; lastWritten = null;
      render();
    });
    $("#save").addEventListener("click", saveFile);
    $("#open").addEventListener("click", () => $("#file").click());
    $("#file").addEventListener("change", async (e) => {
      const f = e.target.files && e.target.files[0];
      e.target.value = "";
      if (!f) return;
      if (f.size > 5 * 1024 * 1024) { alert("That file is too big to be a score."); return; }
      let json;
      try { json = JSON.parse(await f.text()); } catch (err) { alert("That file isn't a CanDuet score."); return; }
      const ops = S.opsFromFile(session, json);
      if (!ops) { alert("That file isn't a CanDuet score."); return; }
      act(ops);
      cursor = null; sel = null; lastWritten = null;
      render();
    });

    const scoreEl = $("#score");
    scoreEl.addEventListener("mousemove", (e) => {
      const h = hit(e);
      if (!h || (h.near && !e.shiftKey)) { drawOverlay(); return; }
      drawOverlay({ geo: h.geo, t: h.tick, p: h.p, rest: restMode });
    });
    scoreEl.addEventListener("mouseleave", () => drawOverlay());
    scoreEl.addEventListener("mousedown", (e) => {
      if (e.button !== 0) return;
      scoreEl.focus();
      const h = hit(e);
      if (!h) return;
      e.preventDefault();
      if (h.near && !e.shiftKey) {
        sel = { id: h.near.g.item.id, pi: h.near.pi };
        cursor = { m: h.geo.id, t: h.near.g.item.t };
        lastWritten = sel.id;
        render();
        return;
      }
      if (e.shiftKey) {
        const at = h.geo.m.items.find((it) => !it.rest && it.t === h.tick) || (h.near && h.near.g.item);
        if (at) { addToChord(at.id, h.p); return; }
      }
      writeAt(h.geo.id, h.tick, [h.p], restMode);
    });

    document.addEventListener("keydown", onKey);
    window.addEventListener("resize", render);
  }

  function toggleTie() {
    const n = selectedNote();
    if (!n) return;
    act([session.setNote(n.id, { m: n.m, t: n.t, d: n.d, p: n.p, tie: !n.tie })]);
  }

  function onKey(e) {
    const tag = (e.target && e.target.tagName) || "";
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    const k = e.key;
    if ((e.ctrlKey || e.metaKey) && (k === "z" || k === "Z")) { e.preventDefault(); if (e.shiftKey) redo(); else undo(); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "y" || k === "Y")) { e.preventDefault(); redo(); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "s" || k === "S")) { e.preventDefault(); saveFile(); return; }
    if (e.ctrlKey || e.metaKey || e.altKey) {
      if (k === "ArrowUp" || k === "ArrowDown") { e.preventDefault(); transposeSelected(k === "ArrowUp" ? 12 : -12); }
      return;
    }
    const durKeys = { 3: 120, 4: 240, 5: 480, 6: 960, 7: 1920 };
    if (durKeys[k]) { baseDur = durKeys[k]; setDurationSelected(); render(); return; }
    if (k === ".") { dotted = !dotted; setDurationSelected(); render(); return; }
    if (k === "0") { if (cursor) writeAt(cursor.m, cursor.t, [], true); return; }
    if (k === "+") { toggleTie(); return; }
    if (k === " ") { e.preventDefault(); playing ? stopPlay() : startPlay(); return; }
    if (k === "n" || k === "N") { midiWrite = !midiWrite; render(); return; }
    if (k === "Delete" || k === "Backspace") { e.preventDefault(); deleteSelected(); return; }
    if (k === "Escape") { sel = null; $("#help").hidden = true; render(); return; }
    if (k === "ArrowUp" || k === "ArrowDown") { e.preventDefault(); transposeSelected(k === "ArrowUp" ? 1 : -1); return; }
    if (k === "ArrowLeft" || k === "ArrowRight") { e.preventDefault(); moveSelection(k === "ArrowLeft" ? -1 : 1); return; }
    const letter = k.toLowerCase();
    if (letter.length === 1 && LETTERS.includes(letter) && cursor) {
      e.preventDefault();
      const m = measureOf(cursor.m);
      if (!m) return;
      const p = pitchForLetter(letter, m);
      if (e.shiftKey && lastWritten && state.notes[lastWritten]) addToChord(lastWritten, p);
      else if (restMode) writeAt(cursor.m, cursor.t, [], true);
      else writeAt(cursor.m, cursor.t, [p], false);
    }
  }

  async function saveFile() {
    const text = JSON.stringify(S.toFile(state), null, 1);
    const name = ((state.meta.title || "Score").replace(/[\\/:*?"<>|]+/g, " ").trim() || "Score") + ".score.json";
    try {
      if (window.showSaveFilePicker) {
        const h = await window.showSaveFilePicker({ suggestedName: name, types: [{ description: "CanDuet score", accept: { "application/json": [".json"] } }] });
        const w = await h.createWritable();
        await w.write(text);
        await w.close();
        $("#status").textContent = "Saved " + h.name;
        return;
      }
    } catch (err) {
      if (err && err.name === "AbortError") return;
    }
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }

  // ---------------------------------------------------------------------------
  // messages from the app
  // ---------------------------------------------------------------------------
  function onHost(raw) {
    let m = raw;
    if (typeof m === "string") { try { m = JSON.parse(m); } catch (e) { return; } }
    if (!m || typeof m !== "object") return;
    switch (m.t) {
      case "init":
        if (typeof m.name === "string" && m.name && m.name !== myName) {
          myName = m.name.slice(0, 80);
          session = S.createSession(state, myName);
        }
        practice = !!m.practice;
        if (typeof m.bpm === "number") setTempo(m.bpm);
        updateStatus();
        break;
      case "peer":
        if (typeof m.data === "string" && m.data.length <= 8000) receivePeer(m.from, m.data);
        break;
      case "peers":
        if (Array.isArray(m.names)) {
          const before = peers.length;
          peers = m.names.filter((x) => typeof x === "string").map((x) => x.slice(0, 80)).slice(0, 12);
          if (peers.length > before) { fullSentAt = 0; fullBackoff = 3000; sendFull(); }
          updateStatus();
        }
        break;
      case "tempo":
        if (typeof m.bpm === "number") setTempo(m.bpm);
        break;
      case "pos":
        if (typeof m.beat === "number") {
          countIn = typeof m.countIn === "number" ? m.countIn : 0;
          if (!m.playing) {
            if (playing) { playing = false; playPos = null; syncPlayButton(); updateStatus(); }
          } else {
            playing = true;
            playPos = m.beat >= 0 ? m.beat + playFrom : null;
            syncPlayButton();
          }
          drawOverlay();
        }
        break;
      case "notes":
        if (Array.isArray(m.e)) {
          for (const ev of m.e.slice(0, 256)) {
            if (Array.isArray(ev) && ev[0] === 0 && ev[3] > 0 && ev[4] === 0 && Number.isInteger(ev[2]) && ev[2] >= 0 && ev[2] <= 127) midiNote(ev[2]);
          }
        }
        break;
    }
  }

  function setTempo(bpm) {
    tempo = Math.max(10, Math.min(400, Math.round(bpm)));
    if (document.activeElement !== $("#tempo")) $("#tempo").value = tempo;
  }

  // fading the "just written by someone else" tint
  setInterval(() => {
    const now = Date.now();
    let any = false;
    for (const [id, t] of remoteFresh) if (now - t > 4000) { remoteFresh.delete(id); any = true; }
    if (any) render();
  }, 1000);

  // #demo=1: a few bars written in, for checking the drawing in a plain browser
  function demoFill() {
    const ids = Object.values(state.measures).sort((a, b) => (a.pos < b.pos ? -1 : 1)).map((m) => m.id);
    const n = (mi, t, d, p, tie = false) => session.setNote(null, { m: ids[mi], t, d, p, tie });
    const ops = [
      session.setMeta({ title: "Demo" }),
      session.setMeasure(ids[0], { key: 1 }),
      n(0, 0, 480, [[0, 4, 0]]), n(0, 480, 240, [[1, 4, 0]]), n(0, 720, 240, [[2, 4, 0]]), n(0, 960, 720, [[3, 4, 1]]), n(0, 1680, 240, [[4, 4, 0]]),
      n(1, 0, 960, [[0, 4, 0], [2, 4, 0], [4, 4, 0]]), n(1, 1440, 480, [[4, 4, 0]], true),
      n(2, 0, 480, [[4, 4, 0]]), n(2, 480, 120, [[5, 4, 0]]), n(2, 600, 120, [[6, 4, 0]]), n(2, 720, 120, [[0, 5, 0]]), n(2, 840, 120, [[1, 5, -1]]),
      session.setMeasure(ids[3], { time: [3, 4] }),
      n(3, 0, 1440, [[0, 5, 0]]),
    ];
    for (const op of ops) S.applyOp(state, op);
  }

  // ---------------------------------------------------------------------------
  fillSelects();
  bind();
  ensureStart();
  if (params.get("demo") === "1") demoFill();
  render();
  updateStatus();
  if (host) {
    host.addEventListener("message", (e) => onHost(e.data));
    send({ t: "ready" });
  }
  // for checks in a plain browser (and tests): the page's internals
  window.CanDuetScorePage = { state, onHost, act, get session() { return session; }, get layout() { return layout; }, get cursor() { return cursor; }, render: draw };
})();
