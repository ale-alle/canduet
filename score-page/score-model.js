// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
//
// CanDuet shared score model (T-080). Pure data, no DOM: the page (score-ui.js) renders it and turns
// what people do into operations; the other person gets the same operations over the lesson's peer
// channel. See doc/TINCAN-SCORE.md.
//
// Rules
// - A score is one staff: an ordered list of measures, and notes placed in a measure by position
//   (ticks from the start of the measure, 480 a quarter note). Rests are never stored: the gaps
//   between notes are shown as rests. So two people writing in different places never clash.
// - Every change is an operation {v, type, id, by, ts, ...}, validated before use whether it comes
//   from this page or the network.
// - Ordering: Lamport time `ts`, ties broken by author, then id. Fields are last-writer-wins.
//   Removals leave a tombstone so a late or repeated "set" can't bring something back. Applying the
//   same operation twice, or in any order, gives the same result, so a whole score can be resent at
//   any time to repair a lost message (see snapshotOps and digest).
// - Everyone can change everything (owner decision 2026-11-17: no locking).
// - If two people write overlapping notes at the same time, the later write is shown and the
//   other note is hidden (still kept, so both sides agree).
// Classic script (not an ES module) so it also loads from file:// pages; CommonJS for tests.

(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.CanDuetScore = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const VERSION = 1;
  const PPQ = 480;  // ticks in a quarter note
  const LIMITS = {
    measures: 400,
    notes: 6000,
    pitches: 10,     // notes in one chord
    removed: 20000,  // tombstones kept
    slurs: 1000,
    id: 64,
    author: 80,
    title: 120,
  };
  // note lengths in ticks: whole .. 32nd, their dotted forms, and triplet quarter, 8th and 16th
  // (three in the time of two: 320, 160, 80)
  const BASE_DURS = [1920, 960, 480, 240, 120, 60];
  const TRIPLET_DURS = [320, 160, 80];
  const DURS = BASE_DURS.concat(BASE_DURS.slice(0, 5).map((d) => d * 1.5), TRIPLET_DURS);
  const isTriplet = (d) => TRIPLET_DURS.includes(d);
  // treble, bass, or piano: a grand staff (treble above bass), each with its own rhythm
  const CLEFS = ["treble", "bass", "piano"];
  const INSTRUMENTS = ["piano", "guitar", "bass"];
  const TIME_DENS = [1, 2, 4, 8, 16];
  const ID_RE = /^[A-Za-z0-9_.:-]+$/;
  const POS_RE = /^[0-9A-Za-z]{1,40}$/;
  const STEP_SEMIS = [0, 2, 4, 5, 7, 9, 11];  // C D E F G A B

  const isStr = (x, max) => typeof x === "string" && x.length <= max;
  const isId = (x) => isStr(x, LIMITS.id) && x.length > 0 && ID_RE.test(x);
  const isInt = (x, lo, hi) => Number.isInteger(x) && x >= lo && x <= hi;
  const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const plainObj = (o) => o !== null && typeof o === "object" && !Array.isArray(o);

  function emptyState() {
    return {
      v: VERSION, clock: 0, resetTs: 0, resetBy: "",
      meta: { clef: "treble", instrument: "piano", title: "" },
      metaTs: {},     // field -> [ts, by]
      measures: {},   // id -> {id, pos, time, key, ts, by, fieldTs}
      notes: {},      // id -> {id, m, t, d, p, tie, st, s, ts, by}   (st: staccato; s: staff, 1 = bass staff of a grand staff)
      slurs: {},      // id -> {id, a: [measure, tick, staff?], b, h, side, o, ts, by, fieldTs}
      removed: {},    // id -> [ts, by]
    };
  }

  // ---------------------------------------------------------------------------
  // pitches
  // ---------------------------------------------------------------------------

  // a pitch is [step 0-6 (C..B), octave -1..9, alter -2..2]
  const validPitch = (p) => Array.isArray(p) && p.length === 3 && isInt(p[0], 0, 6) && isInt(p[1], -1, 9) && isInt(p[2], -2, 2);
  const midiOf = (p) => 12 * (p[1] + 1) + STEP_SEMIS[p[0]] + p[2];

  const SHARP_SPELL = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 0], [3, 0], [3, 1], [4, 0], [4, 1], [5, 0], [5, 1], [6, 0]];
  const FLAT_SPELL = [[0, 0], [1, -1], [1, 0], [2, -1], [2, 0], [3, 0], [4, -1], [4, 0], [5, -1], [5, 0], [6, -1], [6, 0]];
  // MIDI number -> pitch, spelled for the key (sharps for sharp keys and C, flats for flat keys)
  function spell(midi, key) {
    const pc = ((midi % 12) + 12) % 12;
    const [step, alt] = (key < 0 ? FLAT_SPELL : SHARP_SPELL)[pc];
    const oct = Math.floor((midi - STEP_SEMIS[step] - alt) / 12) - 1;
    return [step, oct, alt];
  }
  // steps altered by a key signature: order of sharps F C G D A E B, flats the reverse
  function keyAlter(key, step) {
    const SHARPS = [3, 0, 4, 1, 5, 2, 6];
    if (key > 0) return SHARPS.slice(0, key).includes(step) ? 1 : 0;
    if (key < 0) return SHARPS.slice().reverse().slice(0, -key).includes(step) ? -1 : 0;
    return 0;
  }

  // ---------------------------------------------------------------------------
  // positions between measures (fractional indexing, base 62)
  // ---------------------------------------------------------------------------

  const DIGITS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  // a key strictly between a and b ("" = open end); both sides compute positions locally and the
  // id breaks ties, so two people inserting at the same spot still agree on the order
  function posBetween(a, b) {
    let out = "";
    for (let i = 0; ; i++) {
      const da = i < a.length ? DIGITS.indexOf(a[i]) : 0;
      const db = i < b.length ? DIGITS.indexOf(b[i]) : 62;
      if (db - da > 1) return out + DIGITS[Math.floor((da + db) / 2)];
      out += DIGITS[da];
      if (out.length >= 39) return out + "V";
      if (db - da === 1) {
        // take a's digit and go one deeper, with no upper bound from b any more
        b = "";
      }
    }
  }

  // ---------------------------------------------------------------------------
  // validation
  // ---------------------------------------------------------------------------

  function validTime(t) {
    return t === null || (Array.isArray(t) && t.length === 2 && isInt(t[0], 1, 16) && TIME_DENS.includes(t[1]));
  }
  const validKey = (k) => k === null || isInt(k, -7, 7);

  function validNote(n) {
    if (!plainObj(n)) return false;
    const keys = Object.keys(n);
    for (const k of ["d", "m", "p", "t", "tie"]) if (!keys.includes(k)) return false;
    if (keys.some((k) => !["d", "m", "p", "t", "tie", "st", "s"].includes(k))) return false;
    if (has(n, "st") && n.st !== true) return false;   // staccato: present and true, or absent
    if (has(n, "s") && n.s !== 1) return false;        // staff: 1 (lower staff), or absent
    if (!isId(n.m) || !DURS.includes(n.d) || !isInt(n.t, 0, 16 * 1920) || n.t % 20 !== 0) return false;
    if (typeof n.tie !== "boolean") return false;
    if (!Array.isArray(n.p) || n.p.length < 1 || n.p.length > LIMITS.pitches || !n.p.every(validPitch)) return false;
    const midis = n.p.map(midiOf);
    if (midis.some((m) => m < 0 || m > 127) || new Set(midis).size !== midis.length) return false;
    return true;
  }

  const OP_FIELDS = {
    setMeta: ["changes"],
    addMeasure: ["pos", "time", "key"],
    setMeasure: ["changes"],
    removeMeasure: [],
    setNote: ["note"],
    removeNote: [],
    addSlur: ["slur"],
    setSlur: ["changes"],
    removeSlur: [],
    reset: [],
  };

  // A slur end: [measure id, tick] or [measure id, tick, 1] on the lower staff of a grand staff.
  // h: how far the middle of the arc is moved, -40..40 (4 px steps). side: -1 above the notes,
  // 1 below, 0 chosen by the notes. o: points of the arc moved up or down, [[measure, tick, dy]]
  // with dy in 2 px steps.
  const validAnchor = (a) => Array.isArray(a) && (a.length === 2 || (a.length === 3 && a[2] === 1)) &&
    isId(a[0]) && isInt(a[1], 0, 16 * 1920) && a[1] % 20 === 0;
  const validBulge = (h) => isInt(h, -40, 40);
  const validSide = (x) => x === -1 || x === 0 || x === 1;
  const validOffsets = (o) => Array.isArray(o) && o.length <= 64 &&
    o.every((x) => Array.isArray(x) && x.length === 3 && isId(x[0]) && isInt(x[1], 0, 16 * 1920) && isInt(x[2], -80, 80));
  const SLUR_FIELDS = ["a", "b", "h", "side", "o"];

  // null when the operation is fine, otherwise a short reason
  function validateOp(op) {
    if (!plainObj(op)) return "shape";
    if (op.v !== VERSION) return "version";
    if (!has(OP_FIELDS, op.type)) return "type";
    const want = ["v", "type", "id", "by", "ts"].concat(OP_FIELDS[op.type]).sort().join(",");
    if (Object.keys(op).sort().join(",") !== want) return "fields";
    if (!isId(op.id)) return "id";
    if (!isStr(op.by, LIMITS.author) || op.by.length === 0) return "by";
    if (!isInt(op.ts, 1, 1e12)) return "ts";
    switch (op.type) {
      case "setMeta": {
        const ch = op.changes;
        if (!plainObj(ch) || Object.keys(ch).length === 0) return "changes";
        for (const [k, v] of Object.entries(ch)) {
          if (k === "clef") { if (!CLEFS.includes(v)) return "clef"; }
          else if (k === "instrument") { if (!INSTRUMENTS.includes(v)) return "instrument"; }
          else if (k === "title") { if (!isStr(v, LIMITS.title)) return "title"; }
          else return "changes";
        }
        break;
      }
      case "addMeasure":
        if (!isStr(op.pos, 40) || !POS_RE.test(op.pos)) return "pos";
        if (!validTime(op.time) || !validKey(op.key)) return "measure";
        break;
      case "setMeasure": {
        const ch = op.changes;
        if (!plainObj(ch) || Object.keys(ch).length === 0) return "changes";
        for (const [k, v] of Object.entries(ch)) {
          if (k === "time") { if (!validTime(v)) return "time"; }
          else if (k === "key") { if (!validKey(v)) return "key"; }
          else return "changes";
        }
        break;
      }
      case "setNote":
        if (!validNote(op.note)) return "note";
        break;
      case "addSlur": {
        const sl = op.slur;
        if (!plainObj(sl)) return "slur";
        const keys = Object.keys(sl);
        if (!["a", "b", "h"].every((k) => keys.includes(k)) || keys.some((k) => !SLUR_FIELDS.includes(k))) return "slur";
        if (!validAnchor(sl.a) || !validAnchor(sl.b) || !validBulge(sl.h)) return "slur";
        if (has(sl, "side") && !validSide(sl.side)) return "slur";
        if (has(sl, "o") && !validOffsets(sl.o)) return "slur";
        break;
      }
      case "setSlur": {
        const ch = op.changes;
        if (!plainObj(ch) || Object.keys(ch).length === 0) return "changes";
        for (const [k, v] of Object.entries(ch)) {
          if (k === "a" || k === "b") { if (!validAnchor(v)) return "slur"; }
          else if (k === "h") { if (!validBulge(v)) return "slur"; }
          else if (k === "side") { if (!validSide(v)) return "slur"; }
          else if (k === "o") { if (!validOffsets(v)) return "slur"; }
          else return "changes";
        }
        break;
      }
    }
    return null;
  }

  // ---------------------------------------------------------------------------
  // applying operations
  // ---------------------------------------------------------------------------

  // (ts1, by1, id1) written after (ts2, by2, id2)?
  function later(ts1, by1, ts2, by2, id1, id2) {
    if (ts1 !== ts2) return ts1 > ts2;
    if (by1 !== by2) return by1 > by2;
    return (id1 || "") > (id2 || "");
  }

  function tombstone(state, id, ts, by) {
    const old = state.removed[id];
    if (!old || later(ts, by, old[0], old[1])) state.removed[id] = [ts, by];
    // keep the tombstone list bounded: forget the oldest
    const ids = Object.keys(state.removed);
    if (ids.length > LIMITS.removed) {
      ids.sort((a, b) => state.removed[a][0] - state.removed[b][0]);
      for (const x of ids.slice(0, ids.length - LIMITS.removed)) delete state.removed[x];
    }
  }

  const beforeReset = (state, ts, by) => !later(ts, by, state.resetTs, state.resetBy);

  // Applies an operation; returns true if the state changed. Never throws on bad input.
  function applyOp(state, op) {
    if (validateOp(op) !== null) return false;
    state.clock = Math.max(state.clock, op.ts);

    switch (op.type) {
      case "setMeta": {
        let changed = false;
        for (const [k, v] of Object.entries(op.changes)) {
          const [ts, by] = state.metaTs[k] || [0, ""];
          if (later(op.ts, op.by, ts, by)) {
            state.meta[k] = v;
            state.metaTs[k] = [op.ts, op.by];
            changed = true;
          }
        }
        return changed;
      }
      case "addMeasure": {
        if (state.removed[op.id] || state.measures[op.id]) return false;
        if (beforeReset(state, op.ts, op.by)) return false;
        if (Object.keys(state.measures).length >= LIMITS.measures) return false;
        state.measures[op.id] = {
          id: op.id, pos: op.pos, time: op.time, key: op.key, ts: op.ts, by: op.by,
          fieldTs: { time: [op.ts, op.by], key: [op.ts, op.by] },
        };
        return true;
      }
      case "setMeasure": {
        const m = state.measures[op.id];
        if (!m) return false;
        let changed = false;
        for (const [k, v] of Object.entries(op.changes)) {
          const [ts, by] = m.fieldTs[k];
          if (later(op.ts, op.by, ts, by)) {
            m[k] = v;
            m.fieldTs[k] = [op.ts, op.by];
            changed = true;
          }
        }
        return changed;
      }
      case "removeMeasure": {
        const had = !!state.measures[op.id];
        const knew = !!state.removed[op.id];
        delete state.measures[op.id];
        tombstone(state, op.id, op.ts, op.by);
        // its notes go too (a note arriving later for this measure is refused in setNote)
        for (const n of Object.values(state.notes)) {
          if (n.m === op.id) { delete state.notes[n.id]; tombstone(state, n.id, op.ts, op.by); }
        }
        return had || !knew;
      }
      case "setNote": {
        if (state.removed[op.id]) return false;
        if (beforeReset(state, op.ts, op.by)) return false;
        const old = state.notes[op.id];
        if (old && !later(op.ts, op.by, old.ts, old.by)) return false;
        if (!old && Object.keys(state.notes).length >= LIMITS.notes) return false;
        if (state.removed[op.note.m]) return false;  // its measure is gone
        const n = op.note;
        state.notes[op.id] = { id: op.id, m: n.m, t: n.t, d: n.d, p: n.p.map((x) => x.slice()), tie: n.tie, st: !!n.st, s: n.s === 1 ? 1 : 0, ts: op.ts, by: op.by };
        return true;
      }
      case "addSlur": {
        if (state.removed[op.id] || state.slurs[op.id]) return false;
        if (beforeReset(state, op.ts, op.by)) return false;
        if (Object.keys(state.slurs).length >= LIMITS.slurs) return false;
        const sl = op.slur;
        const ft = {};
        for (const k of SLUR_FIELDS) ft[k] = [op.ts, op.by];
        state.slurs[op.id] = {
          id: op.id, a: sl.a.slice(), b: sl.b.slice(), h: sl.h, side: sl.side || 0,
          o: (sl.o || []).map((x) => x.slice()), ts: op.ts, by: op.by, fieldTs: ft,
        };
        return true;
      }
      case "setSlur": {
        const sl = state.slurs[op.id];
        if (!sl) return false;
        let changed = false;
        for (const [k, v] of Object.entries(op.changes)) {
          const [ts, by] = sl.fieldTs[k];
          if (later(op.ts, op.by, ts, by)) {
            sl[k] = Array.isArray(v) ? JSON.parse(JSON.stringify(v)) : v;
            sl.fieldTs[k] = [op.ts, op.by];
            changed = true;
          }
        }
        return changed;
      }
      case "removeSlur": {
        const had = !!state.slurs[op.id];
        const knew = !!state.removed[op.id];
        delete state.slurs[op.id];
        tombstone(state, op.id, op.ts, op.by);
        return had || !knew;
      }
      case "removeNote": {
        const had = !!state.notes[op.id];
        const knew = !!state.removed[op.id];
        delete state.notes[op.id];
        tombstone(state, op.id, op.ts, op.by);
        return had || !knew;
      }
      case "reset": {
        // everything written before this goes; later writes stay
        if (!later(op.ts, op.by, state.resetTs, state.resetBy)) return false;
        state.resetTs = op.ts;
        state.resetBy = op.by;
        for (const m of Object.values(state.measures)) {
          if (beforeReset(state, m.ts, m.by)) delete state.measures[m.id];
        }
        for (const n of Object.values(state.notes)) {
          if (beforeReset(state, n.ts, n.by) || !state.measures[n.m]) delete state.notes[n.id];
        }
        for (const sl of Object.values(state.slurs)) {
          if (beforeReset(state, sl.ts, sl.by)) delete state.slurs[sl.id];
        }
        return true;
      }
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // making operations (used by the page)
  // ---------------------------------------------------------------------------

  function createSession(state, author) {
    let counter = 0;
    const prefix = Math.random().toString(36).slice(2, 8);
    const base = (type, id) => {
      state.clock += 1;
      return { v: VERSION, type, id, by: author, ts: state.clock };
    };
    const newId = (kind) => `${kind}.${prefix}.${++counter}`;
    return {
      author,
      newId,
      setMeta: (changes) => ({ ...base("setMeta", newId("x")), changes }),
      addMeasure: (pos, time, key) => ({ ...base("addMeasure", newId("m")), pos, time, key }),
      setMeasure: (id, changes) => ({ ...base("setMeasure", id), changes }),
      removeMeasure: (id) => base("removeMeasure", id),
      setNote: (id, note) => ({ ...base("setNote", id || newId("n")), note }),
      removeNote: (id) => base("removeNote", id),
      addSlur: (a, b, h, extra) => ({ ...base("addSlur", newId("s")), slur: { a, b, h, ...(extra || {}) } }),
      setSlur: (id, changes) => ({ ...base("setSlur", id), changes }),
      removeSlur: (id) => base("removeSlur", id),
      reset: () => base("reset", newId("r")),
    };
  }

  // ---------------------------------------------------------------------------
  // reading the score
  // ---------------------------------------------------------------------------

  function measureTicks(time) { return Math.round((time[0] * 4 * PPQ) / time[1]); }

  // Where the triplet group holding a triplet note at t starts: a group lasts three notes (a quarter
  // for triplet 8ths, a half for triplet quarters) and starts on its own length, except triplet
  // quarters, which can start on any beat. The note must fall on the group's grid. null if none.
  function tripletStart(t, d) {
    const glen = d * 3, align = d === 320 ? 480 : glen;
    for (let s = Math.floor(t / align) * align; s > t - glen; s -= align) {
      if (s >= 0 && (t - s) % d === 0) return s;
    }
    return null;
  }

  // compound time (6/8, 9/8, 12/8, 3/8): the beat is a dotted quarter
  const isCompound = (time) => !!time && time[1] === 8 && time[0] % 3 === 0;

  // Rest lengths that fill [from, to) inside a measure, written the usual way for the time
  // signature: each rest starts on a multiple of its own length, so rests show where the beats are.
  // In compound time a whole silent beat is a dotted quarter rest, and rests never cross a beat.
  // In 5/8, 7/8 and the like nothing longer than a quarter rest. (A silent whole bar is one whole
  // rest; view() does that.)
  function fillRests(from, to, time) {
    const out = [];
    const greedy = (a, b, maxD) => {
      let p = a;
      while (p < b) {
        let d = BASE_DURS.find((x) => x <= maxD && p % x === 0 && p + x <= b);
        if (!d) d = Math.min(b - p, 60);
        out.push({ t: p, d });
        p += d;
      }
    };
    if (isCompound(time)) {
      for (let beat = Math.floor(from / 720) * 720; beat < to; beat += 720) {
        const a = Math.max(from, beat), b = Math.min(to, beat + 720);
        if (a === beat && b === beat + 720) out.push({ t: a, d: 720 });
        else if (a < b) greedy(a, b, 240);
      }
      return out;
    }
    greedy(from, to, time && time[1] === 8 ? 480 : 1920);
    return out;
  }

  // rests inside a triplet group [start, start+len): in steps of a third of the group
  function fillTripletRests(from, to, start, len) {
    const out = [];
    const third = len / 3;
    let p = from;
    while (p < to) {
      let d = [2 * third, third, third / 2].find((x) => (p - start) % x === 0 && p + x <= to);
      if (!d) d = to - p;
      out.push({ t: p, d });
      p += d;
    }
    return out;
  }

  // How a note of `total` ticks starting at `t` in a measure of `len` ticks is written: the pieces
  // that go in this measure (tied together), and what's left for the next measures. One piece when
  // it's a written length; otherwise lengths placed like rests (on the beat).
  function notePieces(t, total, len, time) {
    const here = Math.min(total, len - t);
    let pieces;
    if (here <= 0) pieces = [];
    else if (DURS.includes(here) && (here === total || !isTriplet(here))) pieces = [{ t, d: here }];
    else pieces = fillRests(t, t + here, time);
    return { pieces, left: total - Math.max(0, here) };
  }

  // One staff of one bar as it's shown: the notes that win (newest first; a note that doesn't fit or
  // overlaps a newer one is hidden), triplet groups, and the rests between.
  function buildStaff(notes, len, time, si) {
    const cand = notes.slice().sort((a, b) => (later(a.ts, a.by, b.ts, b.by, a.id, b.id) ? -1 : 1));
    const kept = [];
    let hidden = 0;
    for (const n of cand) {
      const fits = n.t + n.d <= len && !kept.some((k) => n.t < k.t + k.d && k.t < n.t + n.d);
      if (fits) kept.push(n); else hidden++;
    }
    kept.sort((a, b) => a.t - b.t);

    // triplet groups: each triplet note belongs to the group of three around it (a triplet 8th
    // to a quarter's worth, a triplet quarter to a half's), placed on that length
    const tuplets = [];
    for (const n of kept) {
      if (!isTriplet(n.d)) continue;
      if (tuplets.some((g) => n.t >= g.start && n.t < g.start + g.len)) continue;
      const glen = n.d * 3;
      const start = tripletStart(n.t, n.d);
      if (start === null || start + glen > len || tuplets.some((g) => start < g.start + g.len && g.start < start + glen)) continue;
      tuplets.push({ start, len: glen });
    }
    tuplets.sort((a, b) => a.start - b.start);
    const groupOf = (t) => tuplets.findIndex((g) => t >= g.start && t < g.start + g.len);

    const items = [];
    const gap = (a, b) => {
      // rests for [a, b): triplet rests inside groups, ordinary rests between them
      let p = a;
      while (p < b) {
        const gi = groupOf(p);
        if (gi >= 0) {
          const g = tuplets[gi];
          const e = Math.min(b, g.start + g.len);
          for (const r of fillTripletRests(p, e, g.start, g.len)) items.push({ rest: true, t: r.t, d: r.d, tup: gi, s: si });
          p = e;
        } else {
          const next = tuplets.find((g) => g.start > p);
          const e = Math.min(b, next ? next.start : b);
          for (const r of fillRests(p, e, time)) items.push({ rest: true, t: r.t, d: r.d, s: si });
          p = e;
        }
      }
    };
    let p = 0;
    for (const n of kept) {
      gap(p, n.t);
      const gi = groupOf(n.t);
      const item = { rest: false, t: n.t, d: n.d, p: n.p, tie: n.tie, st: n.st, id: n.id, by: n.by, s: si };
      if (gi >= 0 && isTriplet(n.d)) item.tup = gi;
      items.push(item);
      p = n.t + n.d;
    }
    if (kept.length === 0) items.push({ rest: true, t: 0, d: len, whole: true, s: si });
    else gap(p, len);
    return { items, tuplets, hidden };
  }

  // The score as it's shown: measures in order with the time and key that apply, each staff's notes
  // and rests (m.staves; m.items is the first staff), and where each measure starts in quarter notes.
  function view(state) {
    const measures = Object.values(state.measures).sort((a, b) =>
      (a.pos < b.pos ? -1 : a.pos > b.pos ? 1 : 0) || (a.id < b.id ? -1 : 1));
    const byMeasure = {};
    for (const n of Object.values(state.notes)) (byMeasure[n.m] = byMeasure[n.m] || []).push(n);
    const staffCount = state.meta.clef === "piano" ? 2 : 1;

    let time = [4, 4], key = 0, beat = 0;
    const out = [];
    for (let i = 0; i < measures.length; i++) {
      const m = measures[i];
      const timeChanged = i === 0 || (m.time !== null && (m.time[0] !== time[0] || m.time[1] !== time[1]));
      const keyChanged = i === 0 ? (m.key !== null && m.key !== 0) : (m.key !== null && m.key !== key);
      if (m.time !== null) time = m.time;
      if (m.key !== null) key = m.key;
      const len = measureTicks(time);

      const staves = [];
      for (let si = 0; si < staffCount; si++) {
        const mine = (byMeasure[m.id] || []).filter((n) => staffCount === 1 || (n.s || 0) === si);
        staves.push(buildStaff(mine, len, time, si));
      }
      out.push({ id: m.id, index: i, time: time.slice(), key, timeChanged, keyChanged, len, beat, staves,
                 items: staves[0].items, tuplets: staves[0].tuplets, hidden: staves.reduce((x, st) => x + st.hidden, 0) });
      beat += len / PPQ;
    }
    // slurs whose two bars both exist, start before end
    const index = {};
    out.forEach((m) => { index[m.id] = m.index; });
    const slurs = [];
    for (const sl of Object.values(state.slurs)) {
      if (!(sl.a[0] in index) || !(sl.b[0] in index)) continue;
      const staffOf = (x) => (staffCount === 2 && x[2] === 1 ? 1 : 0);
      let a = { mi: index[sl.a[0]], t: sl.a[1], s: staffOf(sl.a) }, b = { mi: index[sl.b[0]], t: sl.b[1], s: staffOf(sl.b) };
      if (b.mi < a.mi || (b.mi === a.mi && b.t < a.t)) [a, b] = [b, a];
      if (a.t >= out[a.mi].len || b.t >= out[b.mi].len) continue;   // the bar got shorter
      const o = sl.o.filter((x) => x[0] in index).map((x) => ({ mi: index[x[0]], t: x[1], dy: x[2] }));
      slurs.push({ id: sl.id, a, b, h: sl.h, side: sl.side, o, by: sl.by });
    }
    slurs.sort((x, y) => (x.id < y.id ? -1 : 1));
    return { meta: { ...state.meta }, measures: out, totalBeats: beat, slurs };
  }

  // Note events for playback, in quarter-note beats from the start: {beat, len, midi}. Tied notes
  // of the same pitch are joined.
  function playEvents(v) {
    const events = [];
    const open = {};  // midi -> event still tied
    for (const m of v.measures) {
      for (const it of [].concat(...(m.staves || [{ items: m.items }]).map((st) => st.items))) {
        if (it.rest) continue;
        const beat = m.beat + it.t / PPQ;
        const len = it.d / PPQ;
        for (const p of it.p) {
          const midi = midiOf(p);
          const prev = open[midi];
          if (prev && Math.abs(prev.beat + prev.len - beat) < 1e-6) prev.len += len;
          else events.push((open[midi] = { beat, len, midi, st: !!it.st, s: it.s || 0 }));
          if (!it.tie) delete open[midi];
        }
      }
    }
    // under a slur: legato
    const at = (x) => v.measures[x.mi].beat + x.t / PPQ;
    const ranges = (v.slurs || []).map((sl) => [at(sl.a) - 1e-6, at(sl.b) + 1e-6, sl.a.s || 0]);
    for (const e of events) e.legato = ranges.some(([a, b, si]) => si === e.s && e.beat >= a && e.beat <= b);
    return events.sort((a, b) => a.beat - b.beat || a.midi - b.midi);
  }

  // how long a note actually sounds, in beats: staccato half, slurred notes their full length,
  // others a little detached (so legato can be heard)
  function soundingLength(e) {
    if (e.st) return Math.min(e.len * 0.5, 0.5);
    if (e.legato) return e.len;
    return Math.max(e.len - Math.min(0.08, e.len * 0.12), e.len * 0.85);
  }

  // ---------------------------------------------------------------------------
  // keeping two copies the same
  // ---------------------------------------------------------------------------

  // The whole state as operations. Applying them anywhere is the same as merging, so this is sent
  // when two copies disagree (it's split into small messages by the page).
  function snapshotOps(state) {
    const ops = [];
    const op = (type, id, by, ts, extra) => ({ v: VERSION, type, id, by, ts, ...extra });
    if (state.resetTs > 0) ops.push(op("reset", "r.snap", state.resetBy, state.resetTs, {}));
    for (const [k, [ts, by]] of Object.entries(state.metaTs)) {
      ops.push(op("setMeta", "x.snap." + k, by, ts, { changes: { [k]: state.meta[k] } }));
    }
    for (const m of Object.values(state.measures)) {
      ops.push(op("addMeasure", m.id, m.by, m.ts, { pos: m.pos, time: m.time, key: m.key }));
      for (const k of ["time", "key"]) {
        const [ts, by] = m.fieldTs[k];
        if (ts !== m.ts || by !== m.by) ops.push(op("setMeasure", m.id, by, ts, { changes: { [k]: m[k] } }));
      }
    }
    for (const n of Object.values(state.notes)) {
      const note = { m: n.m, t: n.t, d: n.d, p: n.p, tie: n.tie };
      if (n.st) note.st = true;
      if (n.s === 1) note.s = 1;
      ops.push(op("setNote", n.id, n.by, n.ts, { note }));
    }
    for (const sl of Object.values(state.slurs)) {
      ops.push(op("addSlur", sl.id, sl.by, sl.ts, { slur: { a: sl.a, b: sl.b, h: sl.h, side: sl.side, o: sl.o } }));
      for (const k of SLUR_FIELDS) {
        const [ts, by] = sl.fieldTs[k];
        if (ts !== sl.ts || by !== sl.by) ops.push(op("setSlur", sl.id, by, ts, { changes: { [k]: sl[k] } }));
      }
    }
    for (const [id, [ts, by]] of Object.entries(state.removed)) {
      const type = id.startsWith("m.") ? "removeMeasure" : id.startsWith("s.") ? "removeSlur" : "removeNote";
      ops.push(op(type, id, by, ts, {}));
    }
    return ops;
  }

  // A short fingerprint of what's shown and kept; equal on both sides when they agree.
  function digest(state) {
    const parts = [state.resetTs, state.resetBy, JSON.stringify(state.meta)];
    for (const id of Object.keys(state.measures).sort()) {
      const m = state.measures[id];
      parts.push(id + m.pos + JSON.stringify([m.time, m.key]));
    }
    for (const id of Object.keys(state.notes).sort()) {
      const n = state.notes[id];
      parts.push(id + n.ts + n.by + JSON.stringify([n.m, n.t, n.d, n.p, n.tie, n.st, n.s]));
    }
    for (const id of Object.keys(state.slurs).sort()) {
      const sl = state.slurs[id];
      parts.push(id + JSON.stringify([sl.a, sl.b, sl.h, sl.side, sl.o]));
    }
    const s = parts.join("|");
    let h1 = 0x811c9dc5, h2 = 0x01000193;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      h1 = Math.imul(h1 ^ c, 16777619);
      h2 = Math.imul(h2 ^ c, 2246822507);
    }
    return (h1 >>> 0).toString(16).padStart(8, "0") + (h2 >>> 0).toString(16).padStart(8, "0") + ":" + parts.length;
  }

  // ---------------------------------------------------------------------------
  // saving to a file
  // ---------------------------------------------------------------------------

  // A file holds the score as shown (no history): {format, version, meta, measures:[{time,key,notes}]}
  function toFile(state) {
    const v = view(state);
    return {
      format: "canduet-score", version: 1, meta: v.meta,
      measures: Object.values(state.measures)
        .sort((a, b) => (a.pos < b.pos ? -1 : a.pos > b.pos ? 1 : 0) || (a.id < b.id ? -1 : 1))
        .map((m) => ({
          time: m.time, key: m.key,
          notes: [].concat(...v.measures.find((x) => x.id === m.id).staves.map((st) => st.items)).filter((i) => !i.rest)
            .map((i) => {
              const fn = { t: i.t, d: i.d, p: i.p, tie: i.tie };
              if (i.st) fn.st = true;
              if (i.s === 1) fn.s = 1;
              return fn;
            }),
        })),
      slurs: v.slurs.map((sl) => ({
        a: sl.a.s ? [sl.a.mi, sl.a.t, 1] : [sl.a.mi, sl.a.t], b: sl.b.s ? [sl.b.mi, sl.b.t, 1] : [sl.b.mi, sl.b.t],
        h: sl.h, side: sl.side, o: sl.o.map((x) => [x.mi, x.t, x.dy]),
      })),
    };
  }

  // Operations that replace the score with a file's contents (a reset, then fresh measures and
  // notes). Returns null if the file isn't a valid score.
  function opsFromFile(session, file) {
    if (!plainObj(file) || file.format !== "canduet-score" || file.version !== 1 || !Array.isArray(file.measures)) return null;
    if (file.measures.length === 0 || file.measures.length > LIMITS.measures) return null;
    const ops = [session.reset()];
    const meta = {};
    if (plainObj(file.meta)) {
      if (CLEFS.includes(file.meta.clef)) meta.clef = file.meta.clef;
      if (INSTRUMENTS.includes(file.meta.instrument)) meta.instrument = file.meta.instrument;
      if (isStr(file.meta.title, LIMITS.title)) meta.title = file.meta.title;
    }
    if (Object.keys(meta).length) ops.push(session.setMeta(meta));
    let pos = "";
    let count = 0;
    const ids = [];
    for (const fm of file.measures) {
      if (!plainObj(fm) || !validTime(fm.time) || !validKey(fm.key) || !Array.isArray(fm.notes)) return null;
      pos = posBetween(pos, "");
      const add = session.addMeasure(pos, fm.time, fm.key);
      ops.push(add);
      ids.push(add.id);
      for (const fn of fm.notes) {
        if (++count > LIMITS.notes) return null;
        const note = plainObj(fn) ? { m: add.id, t: fn.t, d: fn.d, p: fn.p, tie: !!fn.tie } : null;
        if (note && fn.st === true) note.st = true;
        if (note && fn.s === 1) note.s = 1;
        if (!validNote(note)) return null;
        ops.push(session.setNote(null, note));
      }
    }
    if (Array.isArray(file.slurs)) {
      for (const fs of file.slurs.slice(0, LIMITS.slurs)) {
        if (!plainObj(fs) || !Array.isArray(fs.a) || !Array.isArray(fs.b)) return null;
        const a = [ids[fs.a[0]], fs.a[1]].concat(fs.a[2] === 1 ? [1] : []);
        const b = [ids[fs.b[0]], fs.b[1]].concat(fs.b[2] === 1 ? [1] : []);
        const extra = {};
        if (validSide(fs.side)) extra.side = fs.side;
        if (Array.isArray(fs.o)) extra.o = fs.o.filter((x) => Array.isArray(x) && x.length === 3).map((x) => [ids[x[0]], x[1], x[2]]);
        const op = session.addSlur(a, b, fs.h, extra);
        if (validateOp(op) !== null) return null;
        ops.push(op);
      }
    }
    return ops;
  }

  return {
    VERSION, PPQ, LIMITS, DURS, BASE_DURS, TRIPLET_DURS, CLEFS, INSTRUMENTS, isTriplet, isCompound,
    emptyState, validateOp, applyOp, createSession, later,
    midiOf, spell, keyAlter, validPitch, posBetween, measureTicks, fillRests, fillTripletRests, notePieces, tripletStart,
    view, playEvents, soundingLength, snapshotOps, digest, toFile, opsFromFile,
  };
});
