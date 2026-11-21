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
  let baseDur = 480, dotted = false, triplet = false, restMode = false, midiWrite = false;
  let cursor = null;     // {m: measure id, t: ticks}
  let sel = null;        // {id: note id, pi: index in its pitches (sorted low..high)}
  let lastWritten = null;  // note id of the last note written here, for Shift+letter chords
  let lastChain = [];      // all the tied pieces of that note
  let undoStack = [], redoStack = [];
  let playing = false, playPos = null, countIn = 0;
  let tempo = 100;
  const remoteFresh = new Map();  // note id -> time it arrived from someone else (shown tinted briefly)

  // triplets: quarter, 8th and 16th only (three in the time of two)
  const tripletOk = () => baseDur === 480 || baseDur === 240 || baseDur === 120;
  const curDur = () => (triplet && tripletOk() ? (baseDur * 2) / 3 : dotted ? baseDur * 1.5 : baseDur);

  // tools: write, select, erase, tie, slur
  let mode = "write";
  const multi = new Set();   // notes selected besides sel (select tool)
  let selSlur = null;        // id of the selected slur
  let slurPreview = null;    // {id, a, b, h} while a slur is being drawn or dragged
  let drag = null;           // what the pointer is doing (see bindPointer)

  // a note's data for setNote, keeping its staccato unless `over` says otherwise
  function nd(n, over = {}) {
    const o = { m: n.m, t: n.t, d: n.d, p: n.p, tie: n.tie, ...over };
    const st = "st" in over ? over.st : n.st;
    const sf = "s" in over ? over.s : n.s;
    delete o.st;
    delete o.s;
    if (st) o.st = true;
    if (sf === 1) o.s = 1;
    return o;
  }
  // every selected note id (the main selection and the others)
  function selectedIds() {
    const ids = new Set(multi);
    if (sel && state.notes[sel.id]) ids.add(sel.id);
    for (const id of ids) if (!state.notes[id]) ids.delete(id);
    return [...ids];
  }

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
          if (n.m === m.id) out.push(session.setNote(null, nd(n, { m: add.id })));
        }
        return out;
      }
      case "setNote": {
        const n = state.notes[op.id];
        if (!n) return [session.removeNote(op.id)];
        return [session.setNote(op.id, nd(n))];
      }
      case "removeNote": {
        const n = state.notes[op.id];
        if (!n) return [];
        return [session.setNote(null, nd(n))];
      }
      case "addSlur": return [session.removeSlur(op.id)];
      case "setSlur": {
        const sl = state.slurs[op.id];
        if (!sl) return [];
        const back = {};
        for (const k of Object.keys(op.changes)) back[k] = sl[k];
        return [session.setSlur(op.id, back)];
      }
      case "removeSlur": {
        const sl = state.slurs[op.id];
        return sl ? [session.addSlur(sl.a, sl.b, sl.h)] : [];
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
    if (opts.merge && undoStack.length) {
      undoStack[undoStack.length - 1] = inverse.concat(undoStack[undoStack.length - 1]);
      redoStack = [];
    } else if (!opts.noUndo) {
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
  // Lines of music grow to fit their highest and lowest notes. VexFlow puts the top staff line 4
  // spaces (40 px) below the stave's y; SPACE is one staff space.
  const SPACE = 10, MIN_ABOVE = 50, MIN_BELOW = 60;

  let layout = [];   // per measure: {id, m (view), x, w, y, stave, ticksX:[[tick,x]], notes:[{item, sn}]}
  let lastView = null;
  let renderQueued = false;

  function durCode(d) {
    if (DUR_CODE[d]) return { code: DUR_CODE[d], dots: 0 };
    if (S.isTriplet(d)) return { code: DUR_CODE[d * 1.5], dots: 0 };
    if (DUR_CODE[d / 1.5]) return { code: DUR_CODE[d / 1.5], dots: 1 };
    return { code: "q", dots: 0 };
  }
  const ACC_GLYPH = { "-2": "bb", "-1": "b", 0: "n", 1: "#", 2: "##" };

  // the length beams group by: a beat (a dotted quarter in 6/8, 9/8, 12/8), a half note in x/2
  function beamUnit(time) {
    if (S.isCompound(time)) return 720;
    if (time[1] === 2) return 960;
    if (time[1] === 16) return 240;
    return 480;
  }

  // staves: one (treble or bass), or two for piano (treble above bass, each its own rhythm)
  const itemsOf = (m, s) => (m.staves[s || 0] || m.staves[0]).items;
  const tupletsOf = (m, s) => (m.staves[s || 0] || m.staves[0]).tuplets;
  const staffCount = () => (state.meta.clef === "piano" ? 2 : 1);
  const clefOf = (s) => (state.meta.clef === "piano" ? (s === 1 ? "bass" : "treble") : state.meta.clef);
  const clefBase = (s) => (clefOf(s || 0) === "bass" ? 16 : 28);  // diatonic index on VexFlow line 0
  const diatonic = (p) => p[1] * 7 + p[0];
  // the drawn bar on staff s
  function geoOf(mId, s) { return layout.find((l) => l.id === mId && l.s === (s || 0)) || layout.find((l) => l.id === mId); }

  // the note before `it` on its staff (in this measure, or the last of the one before) if it's tied into `it`
  function tiedFrom(m, it, prevM) {
    const items = itemsOf(m, it.s);
    const i = items.indexOf(it);
    for (let j = i - 1; j >= 0; j--) {
      const q = items[j];
      if (q.t + q.d === it.t) return !q.rest && q.tie ? q : null;
    }
    if (it.t !== 0 || !prevM) return null;
    const prev = itemsOf(prevM, it.s);
    const last = prev[prev.length - 1];
    return last && !last.rest && last.tie && last.t + last.d === prevM.len ? last : null;
  }
  function vfKey(p) {
    const acc = p[2] === 1 ? "#" : p[2] === 2 ? "##" : p[2] === -1 ? "b" : p[2] === -2 ? "bb" : "";
    return `${LETTERS[p[0]]}${acc}/${p[1]}`;
  }

  function render() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; draw(); });
  }

  // the notes and rests of a slur's staff from its start to its end
  function itemsUnder(v, sl) {
    const out = [];
    for (let mi = sl.a.mi; mi <= sl.b.mi && mi < v.measures.length; mi++) {
      for (const it of itemsOf(v.measures[mi], sl.a.s)) {
        if (mi === sl.a.mi && it.t < sl.a.t) continue;
        if (mi === sl.b.mi && it.t > sl.b.t) continue;
        out.push(it);
      }
    }
    return out;
  }
  // which side a slur goes: the side it was given (dragged to), or above when its notes sit in
  // the top half of the staff, below otherwise
  function slurSide(v, sl) {
    if (sl.side) return sl.side;
    let sum = 0, n = 0;
    for (const it of itemsUnder(v, sl)) if (!it.rest) for (const p of it.p) { sum += (diatonic(p) - clefBase(sl.a.s)) / 2; n++; }
    return n && sum / n >= 3 ? -1 : 1;
  }
  // Notes under a slur point their stems away from it (owner's rule): a slur above means stems
  // down. VexFlow stem directions: 1 up, -1 down, so a slur's side is its notes' stem direction.
  function slurStems(v) {
    const forced = {};
    for (const sl of v.slurs) {
      const side = slurSide(v, sl);
      for (const it of itemsUnder(v, sl)) if (!it.rest) forced[it.id] = side;
    }
    return forced;
  }

  function draw() {
    const v = S.view(state);
    lastView = v;
    const scoreEl = $("#score");
    const overlay = $("#overlay");
    const slurLayer = $("#slurs");
    for (const el of Array.from(scoreEl.children)) if (el !== overlay && el !== slurLayer) el.remove();
    $("#empty").hidden = v.measures.length > 0;
    if (!cursorValid(v)) cursor = v.measures.length ? { m: v.measures[0].id, t: 0, s: 0 } : null;
    syncControls(v);
    layout = [];
    if (!VF || v.measures.length === 0) { slurLayer.innerHTML = ""; drawOverlay(); return; }

    const width = Math.max(320, scoreEl.clientWidth || 800);
    const now = Date.now();
    const nStaves = v.measures[0].staves.length;
    const forced = slurStems(v);

    // build each measure's notes first, to know how wide it wants to be
    const built = v.measures.map((m, i) => buildMeasure(m, now, i > 0 ? v.measures[i - 1] : null, forced));

    // rows: as many measures as fit; the first of each row repeats clef and key
    const rows = [];
    let row = [], used = 0;
    // a grand staff's brace sits left of the staves
    const margin = nStaves === 2 ? 26 : 0, usable = width - margin;
    for (let i = 0; i < built.length; i++) {
      const b = built[i];
      const w = b.minW + b.headW(row.length === 0);
      if (row.length && used + w > usable) { rows.push(row); row = []; used = 0; }
      const w2 = b.minW + b.headW(row.length === 0);
      row.push({ b, w: w2 });
      used += w2;
    }
    if (row.length) rows.push(row);

    const renderer = new VF.Renderer(document.createElement("div"), VF.Renderer.Backends.SVG);
    // Each line's height: room for its highest and lowest notes (staff lines: 1 bottom, 5 top),
    // their stems and triplet numbers. A grand staff has both staves, with the gap between them
    // sized the same way. tops[s] is where staff s's top line is.
    const rowBox = rows.map((r) => {
      const hi = (s) => Math.max(5, ...r.map((c) => c.b.st[s].hi)), lo = (s) => Math.min(1, ...r.map((c) => c.b.st[s].lo));
      const above = Math.max(MIN_ABOVE, (hi(0) - 5) * SPACE + 45);
      if (nStaves === 1) {
        const below = Math.max(MIN_BELOW, (1 - lo(0)) * SPACE + 45);
        return { above, tops: [above], h: above + 4 * SPACE + below };
      }
      const gap = Math.max(70, (1 - lo(0)) * SPACE + 30 + (hi(1) - 5) * SPACE + 30);
      const below = Math.max(MIN_BELOW, (1 - lo(1)) * SPACE + 45);
      const tops = [above, above + 4 * SPACE + gap];
      return { above, tops, split: above + 4 * SPACE + gap / 2, h: tops[1] + 4 * SPACE + below };
    });
    let acc = 0;
    for (const rb of rowBox) { rb.top = acc; acc += rb.h; }
    renderer.resize(width, acc + 20);
    const ctx = renderer.getContext();
    ctx.setFont("Segoe UI", 10);

    const tieQueue = [];
    rows.forEach((r, ri) => {
      const total = r.reduce((s, x) => s + x.w, 0);
      const last = ri === rows.length - 1;
      const stretch = last && total < usable * 0.75 ? Math.min(1.3, usable / total) : usable / total;
      let x = margin;
      r.forEach((cell, ci) => {
        const w = Math.floor(cell.w * stretch) - (ci === r.length - 1 && stretch !== 1 ? 1 : 0);
        const rb = rowBox[ri];
        const geos = drawMeasure(ctx, cell.b, x, rb, w, ci === 0, tieQueue, ci === r.length - 1 && ri < rows.length - 1, ri);
        geos.forEach((geo, s) => {
          // each staff answers clicks in its own band of the line
          const bandTop = s === 0 ? rb.top : rb.top + rb.split;
          const bandH = nStaves === 1 ? rb.h : s === 0 ? rb.split : rb.h - rb.split;
          layout.push({ ...geo, row: ri, rowTop: bandTop, rowH: bandH, lineTop: rb.top, lineH: rb.h });
        });
        x += w;
      });
    });

    // ties: to the next note with the same pitch on the same staff, which may be in the next measure
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
    scoreEl.insertBefore(svg, slurLayer);
    slurLayer.setAttribute("width", width);
    slurLayer.setAttribute("height", acc + 20);
    drawSlurs();
    drawOverlay();
  }

  // one staff of one bar, ready to format: notes with accidentals, triplets, beams, staccato
  function buildStaff(m, s, now, prevM, forced) {
    const clef = clefOf(s);
    const notes = [];
    let hi = 5, lo = 1;
    // Accidentals, worked out here rather than by VexFlow: a sign is shown when the note differs
    // from what's in force for that line and octave (the key signature, or an earlier sign in the
    // bar). A note tied over from the bar before shows no sign and doesn't count as one, so the next
    // note on that line shows its sign again; a note tied within the bar shows none.
    const inForce = {};
    for (const it of itemsOf(m, s)) {
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
        if (forced[it.id]) sn.setStemDirection(forced[it.id]);
        const from = tiedFrom(m, it, prevM);
        pitches.forEach((p, i) => {
          const k = p[0] + "," + p[1];
          const current = k in inForce ? inForce[k] : S.keyAlter(m.key, p[0]);
          const continued = from && from.p.some((q) => S.midiOf(q) === S.midiOf(p));
          if (continued) return;
          if (current !== p[2]) sn.addModifier(new VF.Accidental(ACC_GLYPH[p[2]]), i);
          inForce[k] = p[2];
        });
        if (dots) VF.Dot.buildAndAttach([sn], { all: true });
        const selected = (sel && sel.id === it.id) || multi.has(it.id);
        const fresh = remoteFresh.has(it.id) && now - remoteFresh.get(it.id) < 4000;
        if (selected) sn.setStyle({ fillStyle: "#e5484d", strokeStyle: "#e5484d" });
        else if (fresh) sn.setStyle({ fillStyle: "#1d7fd0", strokeStyle: "#1d7fd0" });
        it.sorted = pitches;
        for (const q of pitches) {
          const line = (diatonic(q) - clefBase(s)) / 2;   // VexFlow line: 1 bottom, 5 top
          if (line > hi) hi = line;
          if (line < lo) lo = line;
        }
      }
      notes.push({ item: it, sn });
    }

    // triplets: a 3:2 bracket over each group (no bracket when the group is one beam)
    const tuplets = [];
    const groups = tupletsOf(m, s) || [];
    groups.forEach((g, gi) => {
      const members = notes.filter((n) => n.item.tup === gi);
      if (!members.length) return;
      const beamable = members.every((n) => !n.item.rest && n.item.d < 320);
      try {
        tuplets.push(new VF.Tuplet(members.map((n) => n.sn), { num_notes: 3, notes_occupied: 2, ratioed: false, bracketed: !beamable }));
      } catch (e) { /* drawn without */ }
    });

    const voice = new VF.Voice({ num_beats: m.time[0], beat_value: m.time[1] }).setMode(VF.Voice.Mode.SOFT);
    voice.addTickables(notes.map((n) => n.sn));

    // Beams: notes shorter than a quarter, joined within a beat (within the group for triplets);
    // a rest, a longer note or the next beat ends the beam. Under a slur the stems keep the
    // direction the slur gives them.
    const beams = [];
    const unit = beamUnit(m.time);
    let run = [], runKey = null;
    const flush = () => {
      if (run.length >= 2) {
        try {
          const f = run.find((n) => forced[n.item.id]);
          if (f) {
            for (const n of run) n.sn.setStemDirection(forced[f.item.id]);
            beams.push(new VF.Beam(run.map((n) => n.sn), false));
          } else beams.push(new VF.Beam(run.map((n) => n.sn), true));
        } catch (e) { /* flags instead */ }
      }
      run = []; runKey = null;
    };
    for (const n of notes) {
      const it = n.item;
      const flagged = !it.rest && (S.isTriplet(it.d) ? it.d < 320 : it.d < 480);
      if (!flagged) { flush(); continue; }
      const g = it.tup !== undefined ? groups[it.tup] : null;
      const key = g ? "t" + it.tup : "b" + Math.floor(it.t / unit);
      const end = g ? g.start + g.len : (Math.floor(it.t / unit) + 1) * unit;
      if (runKey !== null && key !== runKey) flush();
      if (it.t + it.d > end) { flush(); continue; }   // runs past the beat: flag on its own
      run.push(n); runKey = key;
    }
    flush();

    // staccato dots go on the notehead side (after the beams, which can turn stems)
    for (const n of notes) {
      if (n.item.rest || !n.item.st) continue;
      try {
        const up = n.sn.getStemDirection() === 1;
        n.sn.addModifier(new VF.Articulation("a.").setPosition(up ? VF.Modifier.Position.BELOW : VF.Modifier.Position.ABOVE), 0);
      } catch (e) { /* drawn without */ }
    }
    if (tuplets.length) hi += 1.5;   // the "3" above
    return { s, clef, voice, notes, beams, tuplets, hi, lo };
  }

  // a bar: its staves formatted together, so both hands line up in time
  function buildMeasure(m, now, prevM, forced) {
    const st = m.staves.map((_, s) => buildStaff(m, s, now, prevM, forced));
    const fmt = new VF.Formatter();
    for (const sb of st) fmt.joinVoices([sb.voice]);
    let minW = 0;
    try { minW = fmt.preCalculateMinTotalWidth(st.map((sb) => sb.voice)); } catch (e) { minW = Math.max(...st.map((sb) => sb.notes.length)) * 28; }
    minW = Math.max(150, minW * 1.7 + 50);
    const headW = (first) => {
      let w = 0;
      if (first) w += 34 + Math.abs(m.key) * 10;
      if (m.keyChanged) w += (first ? 0 : Math.abs(m.key) * 10 + 14) + (prevM ? Math.abs(prevM.key) * 9 : 0);
      if (m.timeChanged) w += 30;
      return w;
    };
    return { m, prevM, st, fmt, minW, headW };
  }

  function drawMeasure(ctx, b, x, rb, w, first, tieQueue, nextStartsRow, rowIndex) {
    const m = b.m;
    const next = lastView.measures[m.index + 1];
    const staves = b.st.map((sb, s) => {
      const stave = new VF.Stave(x, rb.top + rb.tops[s] - 4 * SPACE, w);
      // a new key cancels the old one with naturals where needed
      const cancel = m.keyChanged && b.prevM ? KEY_NAMES[b.prevM.key] : undefined;
      if (first) stave.addClef(sb.clef);
      if (m.keyChanged) stave.addKeySignature(KEY_NAMES[m.key], cancel);
      else if (first && m.key !== 0) stave.addKeySignature(KEY_NAMES[m.key]);
      if (m.timeChanged) stave.addTimeSignature(m.time.join("/"));
      // the line ends just before a change: show the coming key and time at its end (courtesy)
      if (next && nextStartsRow) {
        try {
          if (next.keyChanged) stave.setEndKeySignature(KEY_NAMES[next.key], KEY_NAMES[m.key]);
          if (next.timeChanged) stave.setEndTimeSignature(next.time.join("/"));
        } catch (e) { /* without */ }
      }
      if (first && s === 0) stave.setMeasure(m.index + 1);
      if (m.index === lastView.measures.length - 1) stave.setEndBarType(VF.Barline.type.END);
      return stave;
    });
    // both staves start their notes at the same x, so the hands line up
    const startX = Math.max(...staves.map((st) => st.getNoteStartX()));
    for (const st of staves) { try { st.setNoteStartX(startX); } catch (e) { /* older VexFlow */ } }
    for (const st of staves) st.setContext(ctx).draw();
    if (staves.length === 2) {
      // the brace and the line joining the staves at the start of a line, bar lines through both
      try {
        const C = VF.StaveConnector;
        if (first) {
          new C(staves[0], staves[1]).setType(C.type.BRACE).setContext(ctx).draw();
          new C(staves[0], staves[1]).setType(C.type.SINGLE_LEFT).setContext(ctx).draw();
        }
        const lastBar = m.index === lastView.measures.length - 1;
        new C(staves[0], staves[1]).setType(lastBar ? C.type.BOLD_DOUBLE_RIGHT : C.type.SINGLE_RIGHT).setContext(ctx).draw();
      } catch (e) { /* drawn without */ }
    }
    const endX = Math.min(...staves.map((st) => st.getNoteEndX()));
    try { b.fmt.format(b.st.map((sb) => sb.voice), Math.max(20, endX - startX - 14)); } catch (e) { /* leave unformatted */ }

    return b.st.map((sb, s) => {
      const stave = staves[s];
      try { sb.voice.draw(ctx, stave); } catch (e) { /* skip */ }
      for (const beam of sb.beams) { try { beam.setContext(ctx).draw(); } catch (e) { /* skip */ } }
      for (const tup of sb.tuplets) { try { tup.setContext(ctx).draw(); } catch (e) { /* skip */ } }

      const ticksX = [];
      const geoNotes = [];
      for (const n of sb.notes) {
        let nx;
        try { nx = n.sn.getNoteHeadBeginX ? n.sn.getNoteHeadBeginX() : n.sn.getAbsoluteX(); } catch (e) { nx = startX; }
        if (n.item.whole) nx = startX + 6;
        ticksX.push([n.item.t, nx]);
        const ys = [];
        try { if (!n.item.rest) for (const yy of n.sn.getYs()) ys.push(yy); } catch (e) { /* none */ }
        geoNotes.push({ item: n.item, sn: n.sn, x: nx, ys, row: rowIndex, s });
      }
      ticksX.push([m.len, endX - 4]);
      const geo = { id: m.id, s, m, x, w, y: stave.getY(), stave, startX, endX, ticksX, notes: geoNotes };

      // ties start here; the end is found when the next measure is drawn
      for (const g of geoNotes) {
        if (g.item.rest || !g.item.tie) continue;
        tieQueue.push({ from: g, to: null, fi: [], li: [], target: findTieTarget(m, g.item) });
      }
      // resolve ties whose targets are drawn by now
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
    });
  }

  // the note right after this one on its staff (same measure, or the first in the next)
  function findTieTarget(m, item) {
    const ms = lastView.measures;
    const inThis = itemsOf(m, item.s).find((i) => !i.rest && i.t === item.t + item.d);
    if (inThis) return inThis;
    if (item.t + item.d >= m.len && ms[m.index + 1]) {
      const first = itemsOf(ms[m.index + 1], item.s)[0];
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
    if (m && (cursor.s || 0) >= m.staves.length) cursor.s = 0;
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
      const g = geoOf(cursor.m, cursor.s);
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
        const line = (diatonic(ghost.p) - clefBase(g.s)) / 2;
        const y = g.stave.getYForNote(line);
        add("ghost", { left: x + "px", top: y + "px" });
        // ledger lines above or below the staff
        for (let l = 0; l >= line; l--) if (Number.isInteger(l)) add("ledger", { left: x + "px", top: g.stave.getYForNote(l) + "px" });
        for (let l = 6; l <= line; l++) add("ledger", { left: x + "px", top: g.stave.getYForNote(l) + "px" });
      }
    }
    if (playing && playPos !== null && playPos >= 0 && lastView) {
      const mv = lastView.measures.find((m) => playPos >= m.beat && playPos < m.beat + m.len / S.PPQ);
      const g = mv && geoOf(mv.id, 0);
      if (g) {
        // across both staves of a grand staff
        const top = g.stave.getYForLine(0), bottom = geoOf(mv.id, staffCount() - 1).stave.getYForLine(4);
        const x = xForTick(g, (playPos - mv.beat) * S.PPQ);
        add("playhead", { left: x + "px", top: top - 18 + "px", height: bottom - top + 36 + "px" });
        followPlayhead(g);
      }
    }
    // the selection box, and the tie tool's line while dragging
    if (drag && drag.type === "box") {
      add("boxsel", { left: Math.min(drag.x0, drag.x1) + "px", top: Math.min(drag.y0, drag.y1) + "px",
                      width: Math.abs(drag.x1 - drag.x0) + "px", height: Math.abs(drag.y1 - drag.y0) + "px" });
    }
    if (drag && drag.type === "tie" && drag.moved) {
      const len = Math.hypot(drag.x1 - drag.x0, drag.y1 - drag.y0);
      const ang = Math.atan2(drag.y1 - drag.y0, drag.x1 - drag.x0);
      add("rubber", { left: drag.x0 + "px", top: drag.y0 + "px", width: len + "px", transform: `rotate(${ang}rad)` });
    }
  }

  let lastScrollRow = -1;
  function followPlayhead(g) {
    const sheet = $("#sheet");
    const scoreTop = $("#score").offsetTop + $("#paper").offsetTop;
    const yIn = scoreTop + g.rowTop;
    if (g.row !== lastScrollRow && (yIn < sheet.scrollTop + 20 || yIn + g.rowH > sheet.scrollTop + sheet.clientHeight)) {
      sheet.scrollTo({ top: Math.max(0, yIn - 40), behavior: "smooth" });
    }
    lastScrollRow = g.row;
  }

  // ---------------------------------------------------------------------------
  // hit testing
  // ---------------------------------------------------------------------------
  function hit(ev) {
    const r = $("#score").getBoundingClientRect();
    const x = ev.clientX - r.left, y = ev.clientY - r.top;
    const geo = layout.find((g) => x >= g.x && x < g.x + g.w && y >= g.rowTop && y < g.rowTop + g.rowH);
    if (!geo) return null;
    // pitch from the height: half a staff space per step
    const sp = geo.stave.getSpacingBetweenLines();
    let line = (geo.stave.getYForNote(0) - y) / sp;
    line = Math.max(-5, Math.min(11, Math.round(line * 2) / 2));
    const d = clefBase(geo.s) + Math.round(line * 2);
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
    const cd = curDur();
    const spots = [];
    if (S.isTriplet(cd)) {
      const align = cd === 320 ? 480 : cd * 3;
      for (let st = 0; st + cd * 3 <= geo.m.len; st += align) for (let k = 0; k < 3; k++) spots.push(st + k * cd);
    } else {
      for (let t = 0; t < geo.m.len; t += Math.min(baseDur, 960)) spots.push(t);
    }
    let tick = 0, bx = Infinity;
    for (const t of spots) {
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

  // which staff a note is drawn on: its own in a grand staff, the only one otherwise
  const staffOfNote = (n) => (staffCount() === 2 ? n.s || 0 : 0);

  // the triplet group an existing triplet note is drawn in (from the view), or where it would be
  function groupOfNote(m, n) {
    const sv = m.staves && (m.staves[staffOfNote(n)] || m.staves[0]);
    const it = sv && sv.items.find((x) => x.id === n.id);
    if (it && it.tup !== undefined) return sv.tuplets[it.tup];
    const s0 = S.tripletStart(n.t, n.d);
    return { start: s0 === null ? n.t : s0, len: n.d * 3 };
  }

  // Operations that clear [t, t+d) in measure m (a measure from view(), or a new one):
  // - notes starting inside go;
  // - a note reaching in from before is shortened, written as tied notes when its new length
  //   isn't one length;
  // - a triplet group holds only its own triplets: writing a triplet (group = its group) clears
  //   plain notes and other groups' triplets from the whole group, and writing a plain note
  //   clears every triplet group it touches.
  function clearOps(m, t, d, group, s) {
    const ops = [];
    const overlaps = (a1, b1, a2, b2) => a1 < b2 && a2 < b1;
    for (const n of Object.values(state.notes)) {
      if (n.m !== m.id || staffOfNote(n) !== (s || 0)) continue;
      const nTrip = S.isTriplet(n.d);
      let hit;
      if (group) {
        if (nTrip) {
          const g = groupOfNote(m, n);
          const same = g.start === group.start && g.len === group.len;
          hit = same ? overlaps(n.t, n.t + n.d, t, t + d) : overlaps(g.start, g.start + g.len, group.start, group.start + group.len);
        } else {
          hit = overlaps(n.t, n.t + n.d, group.start, group.start + group.len);
          if (hit && n.t < group.start) {
            // a plain note reaching into the group is cut back to where the group starts
            pushShortened(ops, m, n, group.start);
            continue;
          }
        }
      } else if (nTrip) {
        const g = groupOfNote(m, n);
        hit = overlaps(g.start, g.start + g.len, t, t + d);
      } else {
        hit = overlaps(n.t, n.t + n.d, t, t + d);
        if (hit && n.t < t) { pushShortened(ops, m, n, t); continue; }
      }
      if (hit) ops.push(session.removeNote(n.id));
    }
    return ops;
  }

  // note n cut to end at `end`, as tied pieces if that isn't one written length
  function pushShortened(ops, m, n, end) {
    const { pieces } = S.notePieces(n.t, end - n.t, m.len, m.time);
    pieces.forEach((pc, i) => {
      const note = nd(n, { t: pc.t, d: pc.d, tie: i < pieces.length - 1 });
      ops.push(i === 0 ? session.setNote(n.id, note) : session.setNote(null, note));
    });
  }

  // the measure after m, from the view, or a new one added at the end (with m's time and key)
  function nextMeasure(m, ops) {
    const ms = lastView.measures;
    const real = ms.find((x) => x.index === m.index + 1);
    if (real && !m.virtual) return { ...real, pos: state.measures[real.id].pos };
    const pos = S.posBetween(m.pos, "");
    const add = session.addMeasure(pos, null, null);
    ops.push(add);
    const empty = () => ({ items: [], tuplets: [] });
    return { id: add.id, index: m.index + 1, len: m.len, time: m.time, key: m.key, pos, virtual: true, staves: [empty(), empty()] };
  }

  // Everything that writing a note (or a rest) of the current length at [mId, t] does. A note longer
  // than what's left of the bar goes on into the next bars as tied notes (bars are added at the end
  // when needed), the way MuseScore does it. Returns {ops, ids, cursor} or null.
  function buildWrite(mId, t, pitches, asRest, s) {
    s = staffCount() === 2 ? s || 0 : 0;
    const m0 = measureOf(mId);
    if (!m0) return null;
    let m = { ...m0, pos: state.measures[m0.id].pos };
    const total = curDur();
    const trip = S.isTriplet(total);
    const ops = [], ids = [];
    const uniq = [];
    for (const p of pitches) if (!uniq.some((q) => S.midiOf(q) === S.midiOf(p))) uniq.push(p);
    const ps = uniq.slice(0, S.LIMITS.pitches);

    let pieces, group = null;
    if (trip) {
      // a triplet goes in the group already there when this spot is on its grid; otherwise it
      // starts a group here (moved on to the next place a group can start, if need be)
      const glen = total * 3, align = total === 320 ? 480 : glen;
      group = (tupletsOf(m, s) || []).find((g) => g.len === glen && t >= g.start && t < g.start + g.len && (t - g.start) % total === 0) || null;
      if (!group) {
        let st = S.tripletStart(t, total);
        if (st === null) { t = Math.ceil(t / align) * align; st = t; }
        group = { start: st, len: glen };
      }
      if (group.start + group.len > m.len) return null;
      pieces = [{ m, t, d: total }];
    } else {
      pieces = [];
      let left = total, at = t, guard = 0;
      while (left > 0 && guard++ < 16) {
        const r = S.notePieces(at, left, m.len, m.time);
        for (const pc of r.pieces) pieces.push({ m, t: pc.t, d: pc.d });
        left = r.left;
        if (left > 0) { m = nextMeasure(m, ops); at = 0; }
      }
    }
    pieces.forEach((pc, i) => {
      ops.push(...clearOps(pc.m, pc.t, pc.d, group, s));
      if (asRest || !ps.length) return;
      const note = { m: pc.m.id, t: pc.t, d: pc.d, p: ps, tie: i < pieces.length - 1 };
      if (s === 1) note.s = 1;
      const op = session.setNote(null, note);
      ops.push(op);
      ids.push(op.id);
    });
    if (!asRest && ps.length && ops.some((o) => o.type === "setNote" && S.validateOp(o) !== null)) return null;
    const lastPc = pieces[pieces.length - 1];
    let next;
    if (lastPc.t + lastPc.d < lastPc.m.len) next = { m: lastPc.m.id, t: lastPc.t + lastPc.d, s };
    else { const nm = nextMeasure(lastPc.m, ops); next = { m: nm.id, t: 0, s }; }
    return { ops, ids, cursor: next };
  }

  function writeAt(mId, t, pitches, asRest, extraOps, s) {
    if (s === undefined) s = cursor ? cursor.s || 0 : 0;
    const w = buildWrite(mId, t, pitches, asRest, s);
    if (!w) return;
    act((extraOps || []).concat(w.ops));
    cursor = w.cursor;
    lastWritten = w.ids[0] || null;
    lastChain = w.ids;
    // the note just written stays selected, so ↑ ↓ and accidentals fix it straight away; note
    // lengths chosen now are for the next note (entry), not this one
    sel = w.ids.length ? { id: w.ids[0], pi: 0, entry: true } : null;
    render();
  }

  // "+" right after writing a note: the same notes again at the cursor, tied to it (MuseScore)
  function tieOnward() {
    const chain = lastChain.map((id) => state.notes[id]).filter(Boolean);
    const last = chain[chain.length - 1];
    if (!last || !cursor || S.isTriplet(curDur())) return false;
    const pitches = last.p;
    const extra = [session.setNote(last.id, nd(last, { tie: true }))];
    writeAt(cursor.m, cursor.t, pitches, false, extra, staffOfNote(last));
    return true;
  }

  // add a pitch to the chord of note `id`
  function addToChord(id, p) {
    const ids = lastChain.includes(id) ? lastChain : [id];
    const ops = [];
    for (const x of ids) {
      const n = state.notes[x];
      if (!n || n.p.some((q) => S.midiOf(q) === S.midiOf(p)) || n.p.length >= S.LIMITS.pitches) continue;
      ops.push(session.setNote(x, nd(n, { p: n.p.concat([p]) })));
    }
    if (!ops.length) return;
    act(ops);
    sel = { id, pi: 0, entry: !!(sel && sel.entry) };
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
    // the same change for every note tied to this one on that pitch (MuseScore does the same),
    // so a tie never ends up joining two different notes
    const oldMidi = S.midiOf(ps[i]);
    const ops = [];
    for (const id of tieChain(n.id, oldMidi)) {
      const c = state.notes[id];
      const cp = c.p.map((q) => (S.midiOf(q) === oldMidi ? np : q));
      if (new Set(cp.map(S.midiOf)).size !== cp.length) continue;
      const op = session.setNote(id, nd(c, { p: cp }));
      if (S.validateOp(op) === null) ops.push(op);
    }
    if (ops.length) act(ops);
    sel = { id: n.id, pi: sortedPitches(state.notes[n.id]).findIndex((q) => S.midiOf(q) === S.midiOf(np)), entry: !!sel.entry };
    render();
    return true;
  }

  // the notes tied to `id` on `midi`, back and forward, in order (ids)
  function tieChain(id, midi) {
    const ms = lastView ? lastView.measures : [];
    const where = (nid) => {
      for (const m of ms) for (const sv of m.staves) { const it = sv.items.find((x) => x.id === nid); if (it) return { m, it }; }
      return null;
    };
    const after = (m, it) => {
      const inBar = itemsOf(m, it.s).find((x) => !x.rest && x.t === it.t + it.d);
      if (inBar) return inBar;
      if (it.t + it.d < m.len) return null;
      const nm = ms[m.index + 1];
      const first = nm && itemsOf(nm, it.s)[0];
      return first && !first.rest && first.t === 0 ? first : null;
    };
    const before = (m, it) => {
      const inBar = itemsOf(m, it.s).find((x) => !x.rest && x.t + x.d === it.t);
      if (inBar) return inBar;
      if (it.t !== 0 || m.index === 0) return null;
      const pm = ms[m.index - 1];
      const pitems = itemsOf(pm, it.s);
      const last = pitems[pitems.length - 1];
      return last && !last.rest && last.t + last.d === pm.len ? last : null;
    };
    const has = (it) => it && it.p.some((q) => S.midiOf(q) === midi);
    const start = where(id);
    if (!start) return [id];
    const chain = [id];
    let cur = start;
    for (let k = 0; k < 64; k++) {   // back
      const b = before(cur.m, cur.it);
      if (!b || !b.tie || !has(b)) break;
      chain.unshift(b.id);
      cur = where(b.id);
    }
    cur = start;
    for (let k = 0; k < 64; k++) {   // forward
      if (!cur.it.tie) break;
      const a = after(cur.m, cur.it);
      if (!a || !has(a)) break;
      chain.push(a.id);
      cur = where(a.id);
    }
    return chain;
  }

  function transposeSelected(semis) {
    const ids = selectedIds();
    if (ids.length > 1) {
      const ops = [];
      for (const id of ids) {
        const n = state.notes[id];
        const m = measureOf(n.m);
        const np = n.p.map((q) => S.spell(S.midiOf(q) + semis, m ? m.key : 0));
        if (np.some((q) => S.midiOf(q) < 12 || S.midiOf(q) > 115)) return true;
        ops.push(session.setNote(id, nd(n, { p: np })));
      }
      act(ops);
      return true;
    }
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
    if (!n || sel.entry || S.isTriplet(curDur()) || S.isTriplet(n.d)) return false;
    const m = measureOf(n.m);
    if (!m) return false;
    const d = fitDur(curDur(), m.len - n.t);
    if (!d || d === n.d) return true;
    const ops = d > n.d ? clearOps(m, n.t + n.d, d - n.d, null).filter((o) => o.id !== n.id) : [];
    ops.push(session.setNote(n.id, nd(n, { d })));
    act(ops);
    return true;
  }

  function deleteSelected() {
    if (selSlur && state.slurs[selSlur]) { act([session.removeSlur(selSlur)]); selSlur = null; return; }
    const ids = selectedIds();
    if (ids.length > 1) {
      act(ids.map((id) => session.removeNote(id)));
      multi.clear(); sel = null;
      return;
    }
    const n = selectedNote();
    if (!n) return;
    const ps = sortedPitches(n);
    if (ps.length > 1) {
      const rest = ps.filter((_, i) => i !== Math.min(sel.pi, ps.length - 1));
      act([session.setNote(n.id, nd(n, { p: rest }))]);
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
    const s = sel && state.notes[sel.id] ? staffOfNote(state.notes[sel.id]) : cursor ? cursor.s || 0 : 0;
    for (const m of lastView.measures) for (const it of itemsOf(m, s)) if (!it.rest) all.push({ m, it });
    if (sel) {
      const i = all.findIndex((x) => x.it.id === sel.id);
      const j = i + dir;
      if (i >= 0 && j >= 0 && j < all.length) { sel = { id: all[j].it.id, pi: 0 }; multi.clear(); cursor = { m: all[j].m.id, t: all[j].it.t, s }; }
    } else if (cursor) {
      // move the cursor by the note length being written
      const m = measureOf(cursor.m);
      if (!m) return;
      let t = cursor.t + dir * Math.min(curDur(), m.len);
      let mi = m.index;
      if (t >= m.len) { if (lastView.measures[mi + 1]) { mi++; t = 0; } else t = cursor.t; }
      if (t < 0) { if (mi > 0) { mi--; const pm = lastView.measures[mi]; t = Math.max(0, pm.len - Math.min(curDur(), pm.len)); } else t = 0; }
      cursor = { m: lastView.measures[mi].id, t, s: cursor.s || 0 };
    }
    render();
  }

  // octave for a letter: the one closest to the note before the cursor
  function pitchForLetter(letter, m) {
    const step = LETTERS.indexOf(letter);
    // the reference is on the cursor's staff: the note just written there, or the one before the cursor
    const s = cursor ? cursor.s || 0 : 0;
    let refNote = lastWritten && state.notes[lastWritten] && staffOfNote(state.notes[lastWritten]) === s ? state.notes[lastWritten] : null;
    if (!refNote && cursor && lastView) { const b = noteBefore(cursor.m, cursor.t, s); refNote = b ? state.notes[b.it.id] : null; }
    const ref = refNote ? sortedPitches(refNote)[0] : null;
    // nothing written yet: around the bottom of the staff (C4..B4 in treble, C3..B3 in bass)
    const refD = ref ? diatonic(ref) : clefBase(cursor ? cursor.s : 0) + 3;
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
      const ps = notes.map((n) => S.spell(n, m.key)).filter((p) => S.validPitch(p));
      if (ps.length) writeAt(cursor.m, cursor.t, ps, restMode);
    }, 60);
  }

  // ---------------------------------------------------------------------------
  // playing
  // ---------------------------------------------------------------------------
  function startPlay() {
    if (!host || !lastView || !lastView.measures.length) return;
    const from = (cursor && measureOf(cursor.m)) || lastView.measures[0];
    const evs = S.playEvents(lastView)
      .filter((e) => e.beat >= from.beat - 1e-9)
      .map((e) => [+(e.beat - from.beat).toFixed(6), +S.soundingLength(e).toFixed(6), e.midi])
      .filter((e) => e[2] >= 0 && e[2] <= 127);
    const quarters = (from.time[0] * 4) / from.time[1];
    send({
      t: "play", events: evs.slice(0, 20000), end: lastView.totalBeats - from.beat,
      bpb: Number.isInteger(quarters) ? quarters : 0, click: $("#click").getAttribute("aria-pressed") === "true",
      program: 0,   // piano (the sound picker is gone for now, owner 2026-11-17)
    });
    playFrom = from.beat;
    playing = true;
    playPos = null;
    syncPlayButton();
  }
  let playFrom = 0;
  function stopPlay() {
    send({ t: "stop" });
    if (rec) finishRecord(playPos !== null ? playPos - playFrom : rec.last);
    playing = false;
    playPos = null;
    syncPlayButton();
    drawOverlay();
  }
  function syncPlayButton() {
    const b = $("#play");
    // round icons (T-089): play becomes stop while it plays; record becomes stop while it records
    const PLAY = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4.5 19.5 12 7 19.5z" fill="currentColor" stroke="none"/></svg>';
    const STOP = '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="6.5" y="6.5" width="11" height="11" rx="1.5" fill="currentColor" stroke="none"/></svg>';
    const REC = '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="4.5" fill="currentColor" stroke="none"/></svg>';
    const on = playing && !rec;
    b.setAttribute("aria-pressed", String(on));
    b.innerHTML = on ? STOP : PLAY;
    b.title = on ? "Stop (Space)" : "Play from the selected bar (Space)";
    b.setAttribute("aria-label", on ? "Stop" : "Play");
    b.disabled = !!rec;
    const r = $("#rec");
    r.setAttribute("aria-pressed", String(!!rec));
    r.innerHTML = rec ? STOP : REC;
    r.title = rec ? "Stop recording (R)" : "Record from your MIDI keyboard from the selected bar, after a count-in (R)";
    r.setAttribute("aria-label", rec ? "Stop recording" : "Record");
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
    $("#triplet").setAttribute("aria-pressed", String(triplet));
    $("#triplet").disabled = !tripletOk();
    $("#rest").setAttribute("aria-pressed", String(restMode));
    $("#midi").setAttribute("aria-pressed", String(midiWrite));
    $("#clef").value = state.meta.clef;
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
    $("#del").disabled = !selectedIds().length && !(selSlur && state.slurs[selSlur]) && !noteAtCursor();
    $("#copy").disabled = !selectedIds().length;
    $("#paste").disabled = !clipboard;
    document.querySelectorAll("[data-mode]").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.mode === mode)));
    document.body.dataset.mode = mode;
    const ids = selectedIds();
    $("#stacc").disabled = ids.length === 0;
    $("#stacc").setAttribute("aria-pressed", String(ids.length > 0 && ids.every((id) => state.notes[id].st)));
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
    else if (rec) s = "● Recording — play now";
    $("#status").textContent = s;
  }
  setInterval(updateStatus, 2000);

  function fixSelection() {
    if (sel && !state.notes[sel.id]) sel = null;
    for (const id of multi) if (!state.notes[id]) multi.delete(id);
    if (selSlur && !state.slurs[selSlur]) selSlur = null;
  }

  function setMode(m) {
    mode = m;
    drag = null;
    slurPreview = null;
    if (m === "write") { multi.clear(); selSlur = null; }
    render();
  }

  function bind() {
    document.querySelectorAll("[data-dur]").forEach((b) => b.addEventListener("click", () => {
      baseDur = +b.dataset.dur;
      if (!setDurationSelected()) render(); else render();
    }));
    $("#dot").addEventListener("click", () => { dotted = !dotted; if (dotted) triplet = false; setDurationSelected(); render(); });
    $("#triplet").addEventListener("click", () => { triplet = !triplet; if (triplet) dotted = false; render(); });
    $("#rest").addEventListener("click", () => { restMode = !restMode; render(); });
    document.querySelectorAll("[data-mode]").forEach((b) => b.addEventListener("click", () => setMode(b.dataset.mode)));
    $("#stacc").addEventListener("click", toggleStaccato);
    document.querySelectorAll("[data-alt]").forEach((b) => b.addEventListener("click", () => setAlterSelected(+b.dataset.alt)));
    $("#del").addEventListener("click", deleteHere);
    $("#rec").addEventListener("click", () => (rec ? stopPlay() : startRecord()));
    $("#copy").addEventListener("click", () => copySelection(false));
    $("#paste").addEventListener("click", paste);
    for (const id of ["countIn", "quant"]) {
      try { const v = localStorage.getItem("score." + id); if (v) $("#" + id).value = v; } catch (err) { /* not kept */ }
      $("#" + id).addEventListener("change", (e) => { try { localStorage.setItem("score." + id, e.target.value); } catch (err) { /* not kept */ } });
    }
    $("#undo").addEventListener("click", undo);
    $("#redo").addEventListener("click", redo);
    $("#clef").addEventListener("change", (e) => act([session.setMeta({ clef: e.target.value })]));
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
    $("#exportXml").addEventListener("click", exportMusicXML);
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

    bindPointer($("#score"));

    document.addEventListener("keydown", onKey);
    window.addEventListener("resize", render);
  }

  // staccato on (or off, when they all have it) for the selected notes
  function toggleStaccato() {
    const ids = selectedIds();
    if (!ids.length) return;
    const on = !ids.every((id) => state.notes[id].st);
    act(ids.map((id) => session.setNote(id, nd(state.notes[id], { st: on }))));
  }

  function toggleTie() {
    const n = selectedNote();
    if (!n) return;
    act([session.setNote(n.id, nd(n, { tie: !n.tie }))]);
  }

  // ---------------------------------------------------------------------------
  // tools: write, select, eraser, tie, slur
  // ---------------------------------------------------------------------------
  const scoreXY = (ev) => {
    const r = $("#score").getBoundingClientRect();
    return { x: ev.clientX - r.left, y: ev.clientY - r.top };
  };

  let flashTimer = null;
  function flash(text) {
    $("#status").textContent = text;
    clearTimeout(flashTimer);
    flashTimer = setTimeout(updateStatus, 3500);
  }

  // the note or rest whose start is nearest the pointer on the staff under it (or on `onStaff`): {m, t, s}
  function anchorAt(ev, onStaff) {
    const { x, y } = scoreXY(ev);
    let geo;
    if (onStaff === undefined) { const h = hit(ev); geo = h && h.geo; }
    else geo = layout.find((g) => g.s === onStaff && x >= g.x && x < g.x + g.w && y >= g.lineTop && y < g.lineTop + g.lineH);
    if (!geo) return null;
    let best = null, bd = Infinity;
    for (const g of geo.notes) {
      const d = Math.abs(g.x + 6 - x);
      if (d < bd) { bd = d; best = g; }
    }
    return best ? { m: geo.id, t: best.item.t, s: geo.s } : null;
  }

  // the drawn note (or rest) at {m, t, s}: {geo, g}
  function drawnAt(m, t, s) {
    const geo = geoOf(m, s);
    if (!geo) return null;
    let best = null, bd = Infinity;
    for (const g of geo.notes) {
      const d = Math.abs(g.item.t - t);
      if (d < bd) { bd = d; best = g; }
    }
    return best ? { geo, g: best } : null;
  }

  const mIndex = (id) => lastView.measures.findIndex((m) => m.id === id);
  const before = (a, b) => {
    const ia = mIndex(a.m), ib = mIndex(b.m);
    return ia < ib || (ia === ib && a.t <= b.t);
  };

  // ---------------------------------------------------------------------------
  // slurs
  // ---------------------------------------------------------------------------

  // the view's slurs with measure ids: {id, a: {m, t, s}, b, h, side, o: [{m, t, dy}]}
  function slursById() {
    if (!lastView) return [];
    const ms = lastView.measures;
    return lastView.slurs.map((sl) => ({
      id: sl.id, a: { m: ms[sl.a.mi].id, t: sl.a.t, s: sl.a.s }, b: { m: ms[sl.b.mi].id, t: sl.b.t, s: sl.b.s },
      h: sl.h, side: sl.side, o: sl.o.map((x) => ({ m: ms[x.mi].id, t: x.t, dy: x.dy })),
    }));
  }
  const toView = (sl) => ({ a: { mi: mIndex(sl.a.m), t: sl.a.t, s: sl.a.s || 0 }, b: { mi: mIndex(sl.b.m), t: sl.b.t, s: sl.a.s || 0 }, side: sl.side });

  // Where a slur is drawn. It joins its notes on the side it's on (stems point away, see
  // slurStems), passes through a point over every note under it, and rises to clear them; h lifts
  // or lowers the middle (it may cross to the other side), and each point can be moved up or down
  // on its own (o). Split in two across a line break.
  function slurShape(sl) {
    let a = sl.a, b = sl.b;
    if (!before(a, b)) [a, b] = [b, a];
    const s = a.s || 0;
    const side = slurSide(lastView, toView({ ...sl, a, b }));
    const ia = mIndex(a.m), ib = mIndex(b.m);
    if (ia < 0 || ib < 0) return null;
    // a point over every note (and the two ends, even on rests)
    const pts = [];
    for (let mi = ia; mi <= ib; mi++) {
      const m = lastView.measures[mi];
      const geo = geoOf(m.id, s);
      if (!geo) return null;
      for (const g of geo.notes) {
        const t = g.item.t;
        if ((mi === ia && t < a.t) || (mi === ib && t > b.t)) continue;
        const end = (mi === ia && t === a.t) || (mi === ib && t === b.t);
        if (g.item.rest && !end) continue;
        let y;
        if (g.item.rest || !g.ys.length) y = geo.stave.getYForLine(side > 0 ? 4 : 0) + side * 10;
        else y = side > 0 ? Math.max(...g.ys) + 9 : Math.min(...g.ys) - 9;
        const off = (sl.o || []).find((x) => x.m === m.id && x.t === t);
        pts.push({ x: g.x + 6, head: y, row: geo.row, m: m.id, t, dy: off ? off.dy : 0, end: end ? (mi === ia && t === a.t ? "a" : "b") : null, geo });
      }
    }
    if (pts.length < 2) return null;
    // per line: an arc between the line's two ends that clears the heads, then h, then each point's own move
    const rows = [];
    for (const p of pts) {
      if (!rows.length || rows[rows.length - 1].row !== p.row) rows.push({ row: p.row, pts: [] });
      rows[rows.length - 1].pts.push(p);
    }
    rows.forEach((r, ri) => {
      const rowGeos = layout.filter((l) => l.row === r.row && l.s === s);
      const x1 = ri === 0 ? r.pts[0].x : Math.min(...rowGeos.map((l) => l.startX)) - 8;
      const x2 = ri === rows.length - 1 ? r.pts[r.pts.length - 1].x : Math.max(...rowGeos.map((l) => l.x + l.w)) - 4;
      const y1 = ri === 0 ? r.pts[0].head : r.pts[0].head, y2 = ri === rows.length - 1 ? r.pts[r.pts.length - 1].head : r.pts[r.pts.length - 1].head;
      const lerp = (x) => y1 + ((y2 - y1) * (x - x1)) / Math.max(1, x2 - x1);
      let need = 0;
      for (const p of r.pts) if (!p.end) need = Math.max(need, side > 0 ? p.head - lerp(p.x) : lerp(p.x) - p.head);
      const H = Math.max(Math.min(26, 8 + (x2 - x1) * 0.04), need + 5);
      const arc = (x) => {
        const u = (x - x1) / Math.max(1, x2 - x1);
        return lerp(x) + side * (H + (sl.h || 0) * 4) * 4 * u * (1 - u);
      };
      r.x1 = x1; r.x2 = x2; r.arc = arc;
      for (const p of r.pts) p.y = (p.end ? p.head : arc(p.x)) + p.dy * 2;
      // the open ends at a line break follow the arc
      r.line = r.pts.map((p) => ({ x: p.x, y: p.y }));
      if (ri > 0) r.line.unshift({ x: x1, y: arc(x1) + r.pts[0].dy * 2, open: true });
      if (ri < rows.length - 1) r.line.push({ x: x2, y: arc(x2) + r.pts[r.pts.length - 1].dy * 2, open: true });
      // fewer than three points: the arc's middle as one more, so it curves
      if (r.line.length === 2) {
        const xm = (r.line[0].x + r.line[1].x) / 2;
        r.line.splice(1, 0, { x: xm, y: arc(xm), mid: true });
      }
    });
    // the middle handle: on the longest line, half way
    const longest = rows.reduce((x, y) => (y.x2 - y.x1 > x.x2 - x.x1 ? y : x));
    const xm = (longest.x1 + longest.x2) / 2;
    const mid = { x: xm, y: smoothY(longest.line, xm) };
    return { side, pts, rows, mid, s };
  }

  // y on the smooth line through pts at x (straight between the two nearest points is close enough
  // for placing a handle)
  function smoothY(line, x) {
    for (let i = 0; i < line.length - 1; i++) {
      if (x >= line[i].x && x <= line[i + 1].x) return line[i].y + ((line[i + 1].y - line[i].y) * (x - line[i].x)) / Math.max(1, line[i + 1].x - line[i].x);
    }
    return line[line.length - 1].y;
  }

  // a smooth curve through points (Catmull-Rom as cubic Béziers), as SVG path commands
  function curveThrough(line, dyAt) {
    const P = line.map((p, i) => ({ x: p.x, y: p.y + (dyAt ? dyAt(i) : 0) }));
    const f = (n) => n.toFixed(1);
    let d = `M${f(P[0].x)} ${f(P[0].y)}`;
    for (let i = 0; i < P.length - 1; i++) {
      const p0 = P[i - 1] || P[i], p1 = P[i], p2 = P[i + 1], p3 = P[i + 2] || p2;
      const c1 = { x: p1.x + (p2.x - p0.x) / 6, y: p1.y + (p2.y - p0.y) / 6 };
      const c2 = { x: p2.x - (p3.x - p1.x) / 6, y: p2.y - (p3.y - p1.y) / 6 };
      d += ` C${f(c1.x)} ${f(c1.y)} ${f(c2.x)} ${f(c2.y)} ${f(p2.x)} ${f(p2.y)}`;
    }
    return d;
  }

  // the filled slur (thin at its real ends, about 3 px in the middle) and its centre line
  function slurPaths(r, side, first, last) {
    const n = r.line.length;
    const thick = (i) => {
      const u = n > 1 ? i / (n - 1) : 0.5;
      let k = Math.sin(Math.PI * Math.min(1, Math.max(0, u))) ;
      if (!first && u < 0.5) k = 1;
      if (!last && u > 0.5) k = 1;
      return -side * 3.2 * Math.max(k, 0.15);   // the inner edge, towards the notes
    };
    const outer = curveThrough(r.line);
    const innerPts = r.line.map((p, i) => ({ x: p.x, y: p.y + thick(i) })).reverse();
    const inner = curveThrough(innerPts).replace(/^M/, "L");
    return { fill: outer + " " + inner + " Z", line: outer };
  }

  function drawSlurs() {
    const layer = $("#slurs");
    layer.innerHTML = "";
    if (!lastView || !layout.length) return;
    const NS = "http://www.w3.org/2000/svg";
    const el = (name, attrs) => {
      const e = document.createElementNS(NS, name);
      for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
      layer.appendChild(e);
      return e;
    };
    const list = slursById().map((sl) => (slurPreview && slurPreview.id === sl.id ? { ...slurPreview } : sl));
    if (slurPreview && slurPreview.id === "new") list.push({ ...slurPreview });
    for (const sl of list) {
      const shape = slurShape(sl);
      if (!shape) continue;
      const editing = slurPreview && slurPreview.id === sl.id;
      const selected = sl.id === selSlur;
      shape.rows.forEach((r, ri) => {
        const p = slurPaths(r, shape.side, ri === 0, ri === shape.rows.length - 1);
        el("path", { d: p.fill, class: "arc" + (editing ? " preview" : selected ? " sel" : "") });
        if (sl.id !== "new") el("path", { d: p.line, class: "hit", "data-slur": sl.id });
      });
      if ((selected || editing) && sl.id !== "new") {
        // a handle on every point: the ends move to other notes, all of them move up and down
        shape.pts.forEach((pt, i) => {
          el("circle", { cx: pt.x, cy: pt.y, r: pt.end ? 5.5 : 4.5, class: "handle" + (pt.end ? " end" : ""), "data-slur": sl.id, "data-handle": pt.end || "p" + i });
        });
        el("circle", { cx: shape.mid.x, cy: shape.mid.y, r: 5.5, class: "handle mid", "data-slur": sl.id, "data-handle": "h" });
      }
    }
  }

  // all notes and rests of staff s in order, as anchors (for sliding a whole slur along)
  function allAnchors(s) {
    const out = [];
    for (const m of lastView.measures) for (const it of itemsOf(m, s)) out.push({ m: m.id, t: it.t, s: s || 0 });
    return out;
  }
  const anchorIndex = (list, a) => {
    let best = 0, bd = Infinity;
    list.forEach((x, i) => {
      if (x.m !== a.m) return;
      const d = Math.abs(x.t - a.t);
      if (d < bd) { bd = d; best = i; }
    });
    return best;
  };
  const anchorArr = (x) => (x.s === 1 ? [x.m, x.t, 1] : [x.m, x.t]);
  const offsetsArr = (o) => o.filter((x) => x.dy !== 0).slice(0, 64).map((x) => [x.m, x.t, Math.max(-80, Math.min(80, x.dy))]);
  function setOffset(o, m, t, dy) {
    const out = o.filter((x) => !(x.m === m && x.t === t));
    if (dy) out.push({ m, t, dy: Math.max(-80, Math.min(80, Math.round(dy))) });
    return out;
  }

  // ---------------------------------------------------------------------------
  // eraser, tie tool
  // ---------------------------------------------------------------------------

  // eraser: the notehead under the pointer (one note of a chord, or the whole note)
  function eraseAt(ev) {
    const h = hit(ev);
    if (!h || !h.near) return;
    const it = h.near.g.item;
    const key = it.id + ":" + h.near.pi;
    if (drag.done.has(key)) return;
    drag.done.add(key);
    const n = state.notes[it.id];
    if (!n) return;
    const ps = sortedPitches(n);
    const op = ps.length > 1 ? session.setNote(n.id, nd(n, { p: ps.filter((_, i) => i !== h.near.pi) })) : session.removeNote(n.id);
    act([op], { merge: !drag.first });
    drag.first = false;
    if (sel && sel.id === n.id) sel = null;
    multi.delete(n.id);
  }

  // where a drawn note is: {m (view), it}
  function placeOf(id) {
    for (const m of lastView.measures) for (const sv of m.staves) { const it = sv.items.find((x) => x.id === id); if (it) return { m, it }; }
    return null;
  }

  // tie tool: may `x` be tied to `y`? Same staff; y starts where x ends (in the next bar if x ends
  // the bar); a pitch in common
  function canTie(x, y) {
    const px = placeOf(x.id), py = placeOf(y.id);
    if (!px || !py || (x.s || 0) !== (y.s || 0)) return false;
    const mx = px.m, my = py.m;
    const next = (mx === my && y.t === x.t + x.d) || (my.index === mx.index + 1 && x.t + x.d === mx.len && y.t === 0);
    return next && x.p.some((q) => y.p.some((r) => S.midiOf(q) === S.midiOf(r)));
  }

  // ---------------------------------------------------------------------------
  // copy and paste, Backspace and Delete
  // ---------------------------------------------------------------------------
  let clipboard = null;   // {notes: [{off, d, p, tie, st, s}], len} with off from the first note, in ticks

  const globalTick = (m, t) => Math.round(m.beat * S.PPQ) + t;

  function copySelection(cut) {
    const ids = selectedIds();
    if (!ids.length) { flash("Select notes first (V, then click or drag a box)."); return; }
    const items = ids.map((id) => {
      const pl = placeOf(id);
      const n = state.notes[id];
      return pl && n ? { g: globalTick(pl.m, pl.it.t), n } : null;
    }).filter(Boolean).sort((a, b) => a.g - b.g);
    const g0 = items[0].g;
    clipboard = {
      notes: items.map(({ g, n }) => ({ off: g - g0, d: n.d, p: n.p.map((q) => q.slice()), tie: n.tie, st: !!n.st, s: staffOfNote(n) })),
      len: Math.max(...items.map(({ g, n }) => g - g0 + n.d)),
    };
    if (cut) { act(ids.map((id) => session.removeNote(id))); sel = null; multi.clear(); }
    flash((cut ? "Cut " : "Copied ") + items.length + (items.length === 1 ? " note" : " notes") + ". Ctrl+V pastes at the cursor.");
  }

  // the bars from the start on, made longer at the end when something is written past it
  function barRunner(ops) {
    const list = lastView.measures.map((m) => ({ ...m, pos: state.measures[m.id].pos, start: globalTick(m, 0) }));
    return {
      at(g) {
        let guard = 0;
        while (g >= list[list.length - 1].start + list[list.length - 1].len && guard++ < 64) {
          const last = list[list.length - 1];
          const nm = nextMeasure(last, ops);
          nm.start = last.start + last.len;
          list.push(nm);
        }
        const m = list.find((x) => g >= x.start && g < x.start + x.len) || list[list.length - 1];
        return { m, t: g - m.start };
      },
    };
  }

  // writes a note of `len` ticks at global tick g on staff s, as tied pieces across bar lines;
  // returns the new ids (null if a triplet can't go there)
  function placeNote(runner, ops, g, len, ps, s, opts) {
    const pieces = [];
    let left = len, pos = g;
    while (left > 0 && pieces.length < 32) {
      const { m, t } = runner.at(pos);
      if (S.isTriplet(left) && (S.tripletStart(t, left) === null || t + left > m.len)) return null;
      const r = S.notePieces(t, left, m.len, m.time);
      if (!r.pieces.length) break;
      for (const pc of r.pieces) pieces.push({ m, t: pc.t, d: pc.d });
      pos += left - r.left;
      left = r.left;
    }
    const ids = [];
    pieces.forEach((pc, i) => {
      if (opts.clear) ops.push(...clearOps(pc.m, pc.t, pc.d, null, s));
      const note = { m: pc.m.id, t: pc.t, d: pc.d, p: ps, tie: i < pieces.length - 1 || !!opts.tie };
      if (opts.st) note.st = true;
      if (s === 1 && staffCount() === 2) note.s = 1;
      const op = session.setNote(null, note);
      if (S.validateOp(op) === null) { ops.push(op); ids.push(op.id); }
    });
    return ids;
  }

  function paste() {
    if (!clipboard || !cursor) { flash("Copy some notes first (Ctrl+C)."); return; }
    const cm = measureOf(cursor.m);
    if (!cm) return;
    const ops = [];
    const runner = barRunner(ops);
    const base = globalTick(cm, cursor.t);
    // one staff copied: it goes on the cursor's staff; both: each stays on its own
    const oneStaff = new Set(clipboard.notes.map((n) => n.s)).size === 1;
    const ids = [];
    let skipped = 0;
    for (const n of clipboard.notes) {
      const s = staffCount() === 2 ? (oneStaff ? cursor.s || 0 : n.s) : 0;
      const got = placeNote(runner, ops, base + n.off, n.d, n.p, s, { clear: true, st: n.st, tie: n.tie });
      if (got) ids.push(...got); else skipped++;
    }
    if (!ids.length) { flash("Nothing could be pasted there."); return; }
    act(ops);
    // the pasted notes are selected; the cursor goes after them
    multi.clear();
    ids.forEach((id, i) => { if (i === 0) sel = { id, pi: 0 }; else multi.add(id); });
    const end = runner.at(base + clipboard.len);
    cursor = { m: end.m.id, t: end.t, s: cursor.s || 0 };
    lastWritten = ids[ids.length - 1]; lastChain = [lastWritten];
    if (skipped) flash(skipped + " triplet " + (skipped === 1 ? "note doesn't" : "notes don't") + " fit there and were left out.");
    render();
  }

  // the note on the cursor's staff that starts at the cursor
  function noteAtCursor() {
    if (!cursor) return null;
    const m = measureOf(cursor.m);
    const it = m && itemsOf(m, cursor.s).find((x) => !x.rest && x.t === cursor.t);
    return it ? state.notes[it.id] : null;
  }
  // the last note on staff s starting before (m, t), searching back through the bars
  function noteBefore(mId, t, s) {
    const ms = lastView.measures;
    for (let mi = mIndex(mId); mi >= 0; mi--) {
      const items = itemsOf(ms[mi], s).filter((x) => !x.rest && (ms[mi].id !== mId || x.t < t));
      if (items.length) return { m: ms[mi], it: items[items.length - 1] };
    }
    return null;
  }

  // Delete: the selection, or the note at the cursor; the cursor stays where it is
  function deleteHere() {
    if (selSlur && state.slurs[selSlur]) { act([session.removeSlur(selSlur)]); selSlur = null; return; }
    let ids = selectedIds();
    if (!ids.length) { const n = noteAtCursor(); if (n) ids = [n.id]; }
    if (!ids.length) return;
    act(ids.map((id) => session.removeNote(id)));
    sel = null; multi.clear();
  }

  // Backspace: the note here (the selected one, or the last one before the cursor) goes, and the
  // cursor moves back to where it was, with the note before it selected, so pressing it again
  // takes notes away one at a time going backwards
  function backspace() {
    if (selSlur && state.slurs[selSlur]) { act([session.removeSlur(selSlur)]); selSlur = null; return; }
    if (multi.size) { deleteHere(); return; }
    let target = null;
    const s = cursor ? cursor.s || 0 : 0;
    if (sel && state.notes[sel.id]) { const pl = placeOf(sel.id); if (pl) target = pl; }
    if (!target && cursor) target = noteBefore(cursor.m, cursor.t, s);
    if (!target) return;
    const ts = target.it.s || 0;
    act([session.removeNote(target.it.id)]);
    cursor = { m: target.m.id, t: target.it.t, s: ts };
    const prev = noteBefore(target.m.id, target.it.t, ts);
    sel = prev ? { id: prev.it.id, pi: 0 } : null;
    lastWritten = prev ? prev.it.id : null;
    lastChain = lastWritten ? [lastWritten] : [];
    render();
  }

  // ---------------------------------------------------------------------------
  // recording from the MIDI keyboard, in time with the click
  // ---------------------------------------------------------------------------
  let rec = null;   // {fromBeat, held: Map(midi -> beat), notes: [{midi, on, off}], stopBeat}

  function startRecord() {
    if (!host) { flash("Recording needs CanDuet (it listens to your MIDI keyboard)."); return; }
    if (playing) stopPlay();
    const from = (cursor && measureOf(cursor.m)) || lastView.measures[0];
    if (!from) return;
    const quarters = (from.time[0] * 4) / from.time[1];
    rec = { fromBeat: from.beat, fromId: from.id, held: new Map(), notes: [], last: 0 };
    send({ t: "record", countIn: +$("#countIn").value, bpb: Number.isInteger(quarters) ? quarters : 0 });
    playFrom = from.beat;
    playing = true;
    playPos = null;
    syncPlayButton();
  }

  // notes from the keyboard while recording: [type, channel, note, value, source, beat]
  function recordEvents(list) {
    for (const ev of list) {
      if (!Array.isArray(ev) || ev[4] !== 0 || typeof ev[5] !== "number") continue;
      const midi = ev[2], beat = ev[5];
      if (!Number.isInteger(midi) || midi < 0 || midi > 127) continue;
      const on = ev[0] === 0 && ev[3] > 0, off = ev[0] === 1 || (ev[0] === 0 && ev[3] === 0);
      if (on) {
        if (beat < -0.5) continue;                  // count-in; a note just before the first beat counts as on it
        if (rec.held.has(midi)) rec.notes.push({ midi, on: rec.held.get(midi), off: beat });
        rec.held.set(midi, Math.max(0, beat));
      } else if (off && rec.held.has(midi)) {
        rec.notes.push({ midi, on: rec.held.get(midi), off: Math.max(beat, rec.held.get(midi)) });
        rec.held.delete(midi);
      }
      rec.last = Math.max(rec.last, beat);
    }
  }

  // Recording stops: each note's attack and release go to the nearest grid line (8ths or 16ths);
  // notes starting together make a chord (as long as its longest note), a note runs at most until
  // the next one starts, and the bars recorded over are replaced. Long notes are written as tied
  // notes; in a grand staff, notes below middle C go on the bass staff.
  function finishRecord(stopBeat) {
    const r = rec;
    rec = null;
    if (!r) return;
    for (const [midi, on] of r.held) r.notes.push({ midi, on, off: Math.max(on, stopBeat) });
    if (!r.notes.length) { flash("Nothing was played."); return; }
    const q = +$("#quant").value;
    const two = staffCount() === 2;
    const groups = new Map();   // "s:start" -> {s, st, en, midis}
    for (const n of r.notes) {
      const st = Math.round((n.on * S.PPQ) / q) * q;
      let en = Math.round((n.off * S.PPQ) / q) * q;
      if (en <= st) en = st + q;
      const s = two && n.midi < 60 ? 1 : 0;
      const k = s + ":" + st;
      const g = groups.get(k) || { s, st, en, midis: new Set() };
      g.en = Math.max(g.en, en);
      g.midis.add(n.midi);
      groups.set(k, g);
    }
    const ops = [];
    const runner = barRunner(ops);
    const base = Math.round(r.fromBeat * S.PPQ);
    // the bars recorded over are cleared first (on the staves that got notes)
    const lastEnd = Math.max(...[...groups.values()].map((g) => g.en), Math.round((stopBeat * S.PPQ) / q) * q);
    const staves = new Set([...groups.values()].map((g) => g.s));
    for (let g = base; g < base + lastEnd;) {
      const { m } = runner.at(g);
      for (const s of staves) ops.push(...clearOps(m, 0, m.len, null, s));
      g = m.start + m.len;
    }
    let count = 0;
    for (const s of staves) {
      const list = [...groups.values()].filter((g) => g.s === s).sort((a, b) => a.st - b.st);
      list.forEach((g, i) => {
        const end = i + 1 < list.length ? Math.min(g.en, list[i + 1].st) : g.en;
        const at = runner.at(base + g.st);
        const ps = [...g.midis].sort((a, b) => a - b).map((midi) => S.spell(midi, at.m.key)).filter(S.validPitch).slice(0, S.LIMITS.pitches);
        const ids = placeNote(runner, ops, base + g.st, end - g.st, ps, s, { clear: false });
        if (ids) count += ids.length ? 1 : 0;
      });
    }
    act(ops);
    flash("Recorded " + count + (count === 1 ? " note." : " notes.") + " Ctrl+Z takes it back.");
    render();
  }

  // ---------------------------------------------------------------------------
  // pointer
  // ---------------------------------------------------------------------------
  function onSlurDown(e) {
    const id = e.target.dataset && e.target.dataset.slur;
    if (!id || e.button !== 0) return;
    e.stopPropagation();
    e.preventDefault();
    $("#score").focus();
    if (mode === "erase") {
      act([session.removeSlur(id)]);
      if (selSlur === id) selSlur = null;
      return;
    }
    if (mode !== "select" && mode !== "slur") return;
    selSlur = id;
    sel = null;
    multi.clear();
    const orig = slursById().find((x) => x.id === id);
    if (!orig) return;
    const shape = slurShape(orig);
    if (!shape) return;
    const handle = e.target.dataset.handle || "body";
    let point = null;
    if (handle[0] === "p") point = shape.pts[+handle.slice(1)];
    drag = { type: "slur-edit", id, handle, point, start: scoreXY(e), from: anchorAt(e, shape.s), orig, side: shape.side, shape };
    $("#score").setPointerCapture(e.pointerId);
    render();
  }

  function onDown(e) {
    if (e.button !== 0) return;
    const scoreEl = $("#score");
    scoreEl.focus();
    const h = hit(e);
    const pt = scoreXY(e);
    if (mode === "write") {
      if (!h) return;
      e.preventDefault();
      if (e.shiftKey) {
        const at = itemsOf(h.geo.m, h.geo.s).find((it) => !it.rest && it.t === h.tick) || (h.near && h.near.g.item);
        if (at) { addToChord(at.id, h.p); return; }
      }
      if (h.near) {
        // clicking a note: the same length and pitch selects it; anything else writes over it
        const it = h.near.g.item;
        if (it.d === curDur() && it.p.some((q) => S.midiOf(q) === S.midiOf(h.p)) && !restMode) {
          sel = { id: it.id, pi: h.near.pi };
          cursor = { m: h.geo.id, t: it.t, s: h.geo.s };
          lastWritten = it.id; lastChain = [it.id];
          render();
          return;
        }
        writeAt(h.geo.id, it.t, [h.p], restMode, null, h.geo.s);
        return;
      }
      writeAt(h.geo.id, h.tick, [h.p], restMode, null, h.geo.s);
      return;
    }
    e.preventDefault();
    if (mode === "select") {
      selSlur = null;
      if (h && h.near) {
        const id = h.near.g.item.id;
        if (e.shiftKey) {
          if (multi.has(id)) multi.delete(id);
          else if (sel && sel.id === id) sel = null;
          else { if (sel) multi.add(sel.id); sel = { id, pi: h.near.pi }; }
        } else { multi.clear(); sel = { id, pi: h.near.pi }; }
        cursor = { m: h.geo.id, t: h.near.g.item.t, s: h.geo.s };
        lastWritten = id; lastChain = [id];
        render();
        return;
      }
      if (!e.shiftKey) { multi.clear(); sel = null; }
      if (h) cursor = { m: h.geo.id, t: h.tick, s: h.geo.s };
      drag = { type: "box", x0: pt.x, y0: pt.y, x1: pt.x, y1: pt.y };
      scoreEl.setPointerCapture(e.pointerId);
      render();
      return;
    }
    if (mode === "erase") {
      drag = { type: "erase", done: new Set(), first: true };
      scoreEl.setPointerCapture(e.pointerId);
      eraseAt(e);
      return;
    }
    if (mode === "tie") {
      if (!h || !h.near) { flash("Start the tie on a note."); return; }
      const g = h.near.g;
      drag = { type: "tie", from: g.item, x0: g.x + 6, y0: g.ys[h.near.pi], x1: pt.x, y1: pt.y, moved: false };
      scoreEl.setPointerCapture(e.pointerId);
      drawOverlay();
      return;
    }
    if (mode === "slur") {
      const a = anchorAt(e);
      if (!a) return;
      selSlur = null;
      drag = { type: "slur-new", a };
      slurPreview = { id: "new", a, b: a, h: 0, side: 0, o: [] };
      scoreEl.setPointerCapture(e.pointerId);
      drawSlurs();
    }
  }

  function onMove(e) {
    const pt = scoreXY(e);
    if (!drag) {
      if (mode !== "write") return;
      const h = hit(e);
      if (!h) { drawOverlay(); return; }
      const t = h.near && !e.shiftKey ? h.near.g.item.t : h.tick;
      drawOverlay({ geo: h.geo, t, p: h.p, rest: restMode });
      return;
    }
    if (drag.type === "box") { drag.x1 = pt.x; drag.y1 = pt.y; drawOverlay(); return; }
    if (drag.type === "erase") { eraseAt(e); return; }
    if (drag.type === "tie") {
      drag.x1 = pt.x; drag.y1 = pt.y;
      if (Math.hypot(drag.x1 - drag.x0, drag.y1 - drag.y0) > 6) drag.moved = true;
      drawOverlay();
      return;
    }
    if (drag.type === "slur-new") {
      const b = anchorAt(e, drag.a.s);
      if (b) { slurPreview = { id: "new", a: drag.a, b, h: 0, side: 0, o: [] }; drawSlurs(); }
      return;
    }
    if (drag.type === "slur-edit") {
      const o = drag.orig;
      const dy = pt.y - drag.start.y;
      let next = { id: o.id, a: o.a, b: o.b, h: o.h, side: o.side, o: o.o };
      if (drag.handle === "a" || drag.handle === "b") {
        // an end goes to the note under the pointer, and up or down to where it's dropped
        const at = anchorAt(e, drag.shape.s);
        if (at) {
          next[drag.handle] = at;
          const D = drawnAt(at.m, at.t, at.s);
          let head = drag.side > 0 ? Math.max(...(D.g.ys.length ? D.g.ys : [D.geo.stave.getYForLine(4) + 10])) + 9
                                    : Math.min(...(D.g.ys.length ? D.g.ys : [D.geo.stave.getYForLine(0) - 10])) - 9;
          next.o = setOffset(o.o.filter((x) => !(x.m === o[drag.handle].m && x.t === o[drag.handle].t)), at.m, at.t, (pt.y - head) / 2);
        }
      } else if (drag.handle === "h") {
        // the middle: up or down, past the notes to the other side too
        next.h = Math.max(-40, Math.min(40, Math.round(o.h + (drag.side * dy) / 4)));
      } else if (drag.point) {
        // a point over a note: up or down
        next.o = setOffset(o.o, drag.point.m, drag.point.t, drag.point.dy + dy / 2);
      } else {
        // the whole slur slides along the notes
        const at = anchorAt(e, drag.shape.s);
        if (at && drag.from) {
          const list = allAnchors(drag.shape.s);
          const delta = anchorIndex(list, at) - anchorIndex(list, drag.from);
          const ia = anchorIndex(list, o.a), ib = anchorIndex(list, o.b);
          const shift = Math.max(-Math.min(ia, ib), Math.min(list.length - 1 - Math.max(ia, ib), delta));
          next.a = list[ia + shift];
          next.b = list[ib + shift];
          next.o = o.o.map((x) => { const j = anchorIndex(list, x) + shift; const y = list[Math.max(0, Math.min(list.length - 1, j))]; return { m: y.m, t: y.t, dy: x.dy }; });
        }
      }
      slurPreview = next;
      drawSlurs();
    }
  }

  function onUp(e) {
    if (!drag) return;
    const d = drag;
    drag = null;
    if (d.type === "box") {
      const x0 = Math.min(d.x0, d.x1), x1 = Math.max(d.x0, d.x1), y0 = Math.min(d.y0, d.y1), y1 = Math.max(d.y0, d.y1);
      if (x1 - x0 > 4 || y1 - y0 > 4) {
        for (const l of layout) for (const g of l.notes) {
          const x = g.x + 6;
          if (g.item.rest || x < x0 || x > x1 || !g.ys.some((y) => y >= y0 && y <= y1)) continue;
          if (!sel) sel = { id: g.item.id, pi: 0 }; else if (sel.id !== g.item.id) multi.add(g.item.id);
        }
      }
      render();
      return;
    }
    if (d.type === "tie") {
      const from = state.notes[d.from.id];
      if (!d.moved) {
        // a click: a tied note comes untied
        if (from && from.tie) act([session.setNote(from.id, nd(from, { tie: false }))]);
        else flash("Drag from this note to the next one of the same pitch.");
        drawOverlay();
        return;
      }
      const h = hit(e);
      const to = h && h.near ? h.near.g.item : null;
      if (to && to.id !== d.from.id) {
        let x = d.from, y = to;
        const px = placeOf(x.id), py = placeOf(y.id);
        if (px && py && !before({ m: px.m.id, t: x.t }, { m: py.m.id, t: y.t })) [x, y] = [y, x];
        if (canTie(x, y)) {
          const n = state.notes[x.id];
          act([session.setNote(n.id, nd(n, { tie: true }))]);
          return;
        }
      }
      flash("A tie joins a note to the very next note of the same pitch.");
      drawOverlay();
      return;
    }
    if (d.type === "slur-new") {
      const pv = slurPreview;
      slurPreview = null;
      if (pv && (pv.a.m !== pv.b.m || pv.a.t !== pv.b.t)) {
        let a = pv.a, b = pv.b;
        if (!before(a, b)) [a, b] = [b, a];
        const op = session.addSlur(anchorArr(a), anchorArr({ ...b, s: a.s }), 0);
        act([op]);
        selSlur = op.id;
      } else flash("Drag from one note to another to draw a slur.");
      render();
      return;
    }
    if (d.type === "slur-edit") {
      const pv = slurPreview;
      slurPreview = null;
      if (pv) {
        const o = d.orig, ch = {};
        const same = (x, y) => x.m === y.m && x.t === y.t;
        let a = pv.a, b = pv.b;
        if (!before(a, b)) [a, b] = [b, a];
        if (same(a, b)) { render(); return; }
        if (!same(a, o.a)) ch.a = anchorArr(a);
        if (!same(b, o.b)) ch.b = anchorArr({ ...b, s: a.s });
        let h = pv.h, side = pv.side || 0, offs = pv.o;
        // pulled through to the other side: the slur goes there, and its notes' stems turn
        if (d.handle === "h") {
          const r = d.shape.rows.reduce((x, y) => (y.x2 - y.x1 > x.x2 - x.x1 ? y : x));
          const xm = (r.x1 + r.x2) / 2;
          const baseline = r.arc(xm) - d.shape.side * ((o.h || 0) * 4);   // the arc without h
          const chord = (r.line[0].y + r.line[r.line.length - 1].y) / 2;
          const bulge = d.shape.side * (baseline - chord) + h * 4;
          if (bulge < -6) { side = -d.shape.side; h = 0; offs = []; }
        }
        if (h !== o.h) ch.h = h;
        if (side !== (o.side || 0)) ch.side = side;
        const oa = offsetsArr(offs), ob = offsetsArr(o.o);
        if (JSON.stringify(oa) !== JSON.stringify(ob)) ch.o = oa;
        if (Object.keys(ch).length) act([session.setSlur(o.id, ch)]);
      }
      render();
      return;
    }
    render();
  }

  function bindPointer(scoreEl) {
    $("#slurs").addEventListener("pointerdown", onSlurDown);
    scoreEl.addEventListener("pointerdown", onDown);
    scoreEl.addEventListener("pointermove", onMove);
    scoreEl.addEventListener("pointerup", onUp);
    scoreEl.addEventListener("pointercancel", () => { drag = null; slurPreview = null; render(); });
    scoreEl.addEventListener("pointerleave", () => { if (!drag) drawOverlay(); });
  }

  function onKey(e) {
    const tag = (e.target && e.target.tagName) || "";
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    const k = e.key;
    if ((e.ctrlKey || e.metaKey) && (k === "z" || k === "Z")) { e.preventDefault(); if (e.shiftKey) redo(); else undo(); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "y" || k === "Y")) { e.preventDefault(); redo(); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "s" || k === "S")) { e.preventDefault(); saveFile(); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "c" || k === "C")) { e.preventDefault(); copySelection(false); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "x" || k === "X")) { e.preventDefault(); copySelection(true); return; }
    if ((e.ctrlKey || e.metaKey) && (k === "v" || k === "V")) { e.preventDefault(); paste(); return; }
    if ((e.ctrlKey || e.metaKey) && k === "3") { e.preventDefault(); triplet = !triplet; if (triplet) dotted = false; render(); return; }
    if (e.ctrlKey || e.metaKey || e.altKey) {
      if (k === "ArrowUp" || k === "ArrowDown") { e.preventDefault(); transposeSelected(k === "ArrowUp" ? 12 : -12); }
      return;
    }
    const durKeys = { 3: 120, 4: 240, 5: 480, 6: 960, 7: 1920 };
    if (durKeys[k]) { baseDur = durKeys[k]; setDurationSelected(); render(); return; }
    if (k === ".") { dotted = !dotted; if (dotted) triplet = false; setDurationSelected(); render(); return; }
    if (k === "0") { if (cursor) writeAt(cursor.m, cursor.t, [], true); return; }
    if (k === "+") { if (!(sel && sel.entry && tieOnward())) toggleTie(); return; }
    if (k === " ") { e.preventDefault(); playing ? stopPlay() : startPlay(); return; }
    if (k === "S" && e.shiftKey) { toggleStaccato(); return; }
    const modeKeys = { w: "write", v: "select", x: "erase", t: "tie", s: "slur" };
    if (modeKeys[k]) { setMode(modeKeys[k]); return; }
    if (k === "n" || k === "N") { midiWrite = !midiWrite; render(); return; }
    if (k === "Delete") { e.preventDefault(); deleteHere(); return; }
    if (k === "Backspace") { e.preventDefault(); backspace(); return; }
    if (k === "r" || k === "R") { rec ? stopPlay() : startRecord(); return; }
    if (k === "Escape") {
      if (drag) { drag = null; slurPreview = null; render(); return; }
      if (mode !== "select") { setMode("select"); return; }
      sel = null; multi.clear(); selSlur = null; $("#help").hidden = true; render(); return;
    }
    if (k === "ArrowUp" || k === "ArrowDown") {
      e.preventDefault();
      // nothing selected in a grand staff: the cursor goes to the other staff
      if (!selectedIds().length && staffCount() === 2 && cursor) { cursor = { ...cursor, s: k === "ArrowUp" ? 0 : 1 }; render(); return; }
      transposeSelected(k === "ArrowUp" ? 1 : -1);
      return;
    }
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

  const fileBase = () => (state.meta.title || "Score").replace(/[\\/:*?"<>|]+/g, " ").trim() || "Score";

  // CanDuet's own file: everything, to open again here
  function saveFile() {
    return saveText(JSON.stringify(S.toFile(state), null, 1), fileBase() + ".score.json", "CanDuet score", "application/json", ".json");
  }

  // MusicXML, the standard other notation programs open (MuseScore, Sibelius, Dorico, Finale)
  function exportMusicXML() {
    const xml = S.toMusicXML(state, { tempo, date: new Date().toISOString().slice(0, 10) });
    return saveText(xml, fileBase() + ".musicxml", "MusicXML", "application/vnd.recordare.musicxml+xml", ".musicxml");
  }

  async function saveText(text, name, description, mime, ext) {
    try {
      if (window.showSaveFilePicker) {
        const h = await window.showSaveFilePicker({ suggestedName: name, types: [{ description, accept: { [mime]: [ext] } }] });
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
    a.href = URL.createObjectURL(new Blob([text], { type: mime }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }

  // ---------------------------------------------------------------------------
  // messages from the app
  // ---------------------------------------------------------------------------
  // the dock icons (T-089): docked, pop out and close; in its own window, dock in when there's a lesson
  const DOCK_ICONS = {
    popOut: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M14 4h6v6M20 4l-8 8M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/></svg>',
    dockIn: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 15h18M12 6.5v5.5M9 9.5l3 3 3-3"/></svg>',
  };
  function showDock(docked, canDock) {
    const b = $("#dockBtn");
    b.innerHTML = docked ? DOCK_ICONS.popOut : DOCK_ICONS.dockIn;
    b.title = docked ? "Pop out: a window of its own" : "Dock in the lesson window";
    b.setAttribute("aria-label", b.title);
    b.hidden = !(docked || canDock);
    $("#closeBtn").hidden = !docked;   // its own window has the title bar's close
  }
  if (host) {
    $("#soundBtn").addEventListener("click", () => send({ t: "soundMenu" }));
    $("#dockBtn").addEventListener("click", () => send({ t: "dockToggle" }));
    $("#closeBtn").addEventListener("click", () => send({ t: "dockClose" }));
  }

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
      case "sound": {
        const b = $("#soundBtn");
        const name = typeof m.name === "string" && m.name ? m.name.slice(0, 120) : "built-in";
        b.title = "Sound: " + name + ". Click to choose the built-in sounds or a VST3 instrument";
        b.setAttribute("aria-label", "Sound: " + name);
        b.hidden = false;
        break;
      }
      case "dock":
        showDock(!!m.docked, !!m.canDock);
        break;
      case "pos":
        if (typeof m.beat === "number") {
          countIn = typeof m.countIn === "number" ? m.countIn : 0;
          if (!m.playing) {
            if (rec) finishRecord(playPos !== null ? playPos - playFrom : rec.last);
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
        if (Array.isArray(m.e) && rec) recordEvents(m.e.slice(0, 256));
        if (Array.isArray(m.e)) {
          for (const ev of m.e.slice(0, 256)) {
            if (rec) continue;
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

  // #demo=2: a test sheet of the notation rules (doc/TINCAN-SCORE.md lists what each bar shows)
  function demoSheet() {
    const ops = [session.reset(), session.setMeta({ title: "Notation test sheet" })];
    const bars = [
      [[4, 4], 1], [null, 0], [null, null], [null, null], [[6, 8], null], [null, null],
      [[3, 4], null], [[4, 4], null], [null, -3], [[2, 2], null], [[4, 4], 0], [null, null],
    ];
    const ids = [];
    let pos = "";
    for (const [time, key] of bars) {
      pos = S.posBetween(pos, "");
      const op = session.addMeasure(pos, time, key);
      ops.push(op);
      ids.push(op.id);
    }
    const P = (name) => {   // "F#4", "Bb3", "C5"
      const step = "CDEFGAB".indexOf(name[0]);
      const alt = name.includes("##") ? 2 : name.includes("#") ? 1 : name.includes("bb") ? -2 : /^.b/.test(name) ? -1 : 0;
      return [step, +name.slice(-1), alt];
    };
    const n = (b, t, d, names, tie = false) => ops.push(session.setNote(null, { m: ids[b], t, d, p: names.split(" ").map(P), tie }));
    // 1 (G major): dotted rhythms and sixteenths
    n(0, 0, 360, "G4"); n(0, 360, 120, "A4"); n(0, 480, 240, "B4"); n(0, 720, 120, "C5"); n(0, 840, 120, "D5");
    n(0, 960, 120, "E5"); n(0, 1080, 120, "D5"); n(0, 1200, 240, "C5"); n(0, 1440, 240, "B4"); n(0, 1680, 240, "A4");
    // 2 (C major, naturals cancel G major): accidentals in force for the bar, per octave
    n(1, 0, 240, "F#4"); n(1, 240, 240, "F4"); n(1, 480, 240, "F#4"); n(1, 720, 240, "F#5");
    n(1, 960, 480, "Bb4"); n(1, 1440, 480, "Bb4");
    // 3: syncopation, a chord with a second, a G# tied over the bar line
    n(2, 0, 240, "C4 D4 E4"); n(2, 240, 480, "E4"); n(2, 720, 240, "F4"); n(2, 960, 480, "A3"); n(2, 1440, 480, "G#4", true);
    // 4: the tied G# shows no sharp, the next G# shows it again; high ledger notes
    n(3, 0, 480, "G#4"); n(3, 480, 480, "G#4"); n(3, 960, 480, "C6"); n(3, 1440, 480, "A5 C6");
    // 5 (6/8): beamed in dotted-quarter beats, a dotted quarter
    n(4, 0, 240, "C5"); n(4, 240, 240, "D5"); n(4, 480, 240, "E5"); n(4, 720, 720, "F5");
    // 6: eighth, eighth rest, eighth, then a silent beat (dotted quarter rest)
    n(5, 0, 240, "G4"); n(5, 480, 240, "A4");
    // 7 (3/4): triplet eighths, a quarter, a triplet with a rest in the middle
    n(6, 0, 160, "C5"); n(6, 160, 160, "D5"); n(6, 320, 160, "E5"); n(6, 480, 480, "F5");
    n(6, 960, 160, "G5"); n(6, 1280, 160, "E5");
    // 8 (4/4): triplet quarters over two beats, then a half note
    n(7, 0, 320, "C5"); n(7, 320, 320, "B4"); n(7, 640, 320, "A4"); n(7, 960, 960, "G4");
    // 9 (Eb major): no signs for Eb and Ab; E natural needs one
    n(8, 0, 480, "Eb4"); n(8, 480, 480, "Ab4"); n(8, 960, 480, "E4"); n(8, 1440, 480, "Eb5");
    // 10 (2/2): eighths beamed by the half bar
    for (let i = 0; i < 8; i++) n(9, i * 240, 240, ["C5", "D5", "E5", "F5", "G5", "F5", "E5", "D5"][i]);
    // 11 (4/4, C major): a whole note tied into the next bar
    n(10, 0, 1920, "C4 E4 G4", true);
    n(11, 0, 960, "C4 E4 G4"); n(11, 960, 240, "D4"); n(11, 1200, 240, "E4");
    for (const op of ops) S.applyOp(state, op);
  }

  // ---------------------------------------------------------------------------
  fillSelects();
  bind();
  ensureStart();
  if (params.get("demo") === "1") demoFill();
  if (params.get("demo") === "2") demoSheet();
  render();
  updateStatus();
  if (host) {
    host.addEventListener("message", (e) => onHost(e.data));
    send({ t: "ready" });
  }
  // for checks in a plain browser (and tests): the page's internals
  window.CanDuetScorePage = { state, onHost, act, get session() { return session; }, get layout() { return layout; }, get cursor() { return cursor; }, render: draw };
})();
