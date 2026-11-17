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
    id: 64,
    author: 80,
    title: 120,
  };
  // note lengths in ticks: whole .. 32nd, and their dotted forms
  const BASE_DURS = [1920, 960, 480, 240, 120, 60];
  const DURS = BASE_DURS.concat(BASE_DURS.slice(0, 5).map((d) => d * 1.5));
  const CLEFS = ["treble", "bass"];
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
      notes: {},      // id -> {id, m, t, d, p, tie, ts, by}
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
    const keys = Object.keys(n).sort().join(",");
    if (keys !== "d,m,p,t,tie") return false;
    if (!isId(n.m) || !DURS.includes(n.d) || !isInt(n.t, 0, 16 * 1920) || n.t % 60 !== 0) return false;
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
    reset: [],
  };

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
        state.notes[op.id] = { id: op.id, m: n.m, t: n.t, d: n.d, p: n.p.map((x) => x.slice()), tie: n.tie, ts: op.ts, by: op.by };
        return true;
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
      reset: () => base("reset", newId("r")),
    };
  }

  // ---------------------------------------------------------------------------
  // reading the score
  // ---------------------------------------------------------------------------

  function measureTicks(time) { return Math.round((time[0] * 4 * PPQ) / time[1]); }

  // Rest lengths that fill [from, to) inside a measure: the longest length that starts on a
  // multiple of itself, so rests line up with the beat.
  function fillRests(from, to) {
    const out = [];
    let p = from;
    while (p < to) {
      let d = BASE_DURS.find((x) => p % x === 0 && p + x <= to);
      if (!d) d = 60;
      if (p + d > to) d = to - p;  // only happens with odd lengths; never longer than the gap
      out.push({ t: p, d });
      p += d;
    }
    return out;
  }

  // The score as it's shown: measures in order with the time and key that apply, the notes that
  // win (sorted), the rests between them, and where each measure starts in quarter notes.
  function view(state) {
    const measures = Object.values(state.measures).sort((a, b) =>
      (a.pos < b.pos ? -1 : a.pos > b.pos ? 1 : 0) || (a.id < b.id ? -1 : 1));
    const byMeasure = {};
    for (const n of Object.values(state.notes)) (byMeasure[n.m] = byMeasure[n.m] || []).push(n);

    let time = [4, 4], key = 0, beat = 0;
    const out = [];
    for (let i = 0; i < measures.length; i++) {
      const m = measures[i];
      const timeChanged = i === 0 || (m.time !== null && (m.time[0] !== time[0] || m.time[1] !== time[1]));
      const keyChanged = i === 0 ? (m.key !== null && m.key !== 0) : (m.key !== null && m.key !== key);
      if (m.time !== null) time = m.time;
      if (m.key !== null) key = m.key;
      const len = measureTicks(time);

      // newest writes first; keep a note only if it fits and doesn't overlap a newer one
      const cand = (byMeasure[m.id] || []).slice().sort((a, b) => (later(a.ts, a.by, b.ts, b.by, a.id, b.id) ? -1 : 1));
      const kept = [];
      let hidden = 0;
      for (const n of cand) {
        const fits = n.t + n.d <= len && !kept.some((k) => n.t < k.t + k.d && k.t < n.t + n.d);
        if (fits) kept.push(n); else hidden++;
      }
      kept.sort((a, b) => a.t - b.t);

      const items = [];
      let p = 0;
      for (const n of kept) {
        for (const r of fillRests(p, n.t)) items.push({ rest: true, t: r.t, d: r.d });
        items.push({ rest: false, t: n.t, d: n.d, p: n.p, tie: n.tie, id: n.id, by: n.by });
        p = n.t + n.d;
      }
      const wholeRest = kept.length === 0;
      if (wholeRest) items.push({ rest: true, t: 0, d: len, whole: true });
      else for (const r of fillRests(p, len)) items.push({ rest: true, t: r.t, d: r.d });

      out.push({ id: m.id, index: i, time: time.slice(), key, timeChanged, keyChanged, len, beat, items, hidden });
      beat += len / PPQ;
    }
    return { meta: { ...state.meta }, measures: out, totalBeats: beat };
  }

  // Note events for playback, in quarter-note beats from the start: {beat, len, midi}. Tied notes
  // of the same pitch are joined.
  function playEvents(v) {
    const events = [];
    const open = {};  // midi -> event still tied
    for (const m of v.measures) {
      for (const it of m.items) {
        if (it.rest) continue;
        const beat = m.beat + it.t / PPQ;
        const len = it.d / PPQ;
        for (const p of it.p) {
          const midi = midiOf(p);
          const prev = open[midi];
          if (prev && Math.abs(prev.beat + prev.len - beat) < 1e-6) prev.len += len;
          else events.push((open[midi] = { beat, len, midi }));
          if (!it.tie) delete open[midi];
        }
      }
    }
    return events.sort((a, b) => a.beat - b.beat || a.midi - b.midi);
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
      ops.push(op("setNote", n.id, n.by, n.ts, { note: { m: n.m, t: n.t, d: n.d, p: n.p, tie: n.tie } }));
    }
    for (const [id, [ts, by]] of Object.entries(state.removed)) {
      ops.push(op(id.startsWith("m.") ? "removeMeasure" : "removeNote", id, by, ts, {}));
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
      parts.push(id + n.ts + n.by + JSON.stringify([n.m, n.t, n.d, n.p, n.tie]));
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
          notes: v.measures.find((x) => x.id === m.id).items.filter((i) => !i.rest).map((i) => ({ t: i.t, d: i.d, p: i.p, tie: i.tie })),
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
    for (const fm of file.measures) {
      if (!plainObj(fm) || !validTime(fm.time) || !validKey(fm.key) || !Array.isArray(fm.notes)) return null;
      pos = posBetween(pos, "");
      const add = session.addMeasure(pos, fm.time, fm.key);
      ops.push(add);
      for (const fn of fm.notes) {
        if (++count > LIMITS.notes) return null;
        const note = plainObj(fn) ? { m: add.id, t: fn.t, d: fn.d, p: fn.p, tie: !!fn.tie } : null;
        if (!validNote(note)) return null;
        ops.push(session.setNote(null, note));
      }
    }
    return ops;
  }

  return {
    VERSION, PPQ, LIMITS, DURS, BASE_DURS, CLEFS, INSTRUMENTS,
    emptyState, validateOp, applyOp, createSession, later,
    midiOf, spell, keyAlter, validPitch, posBetween, measureTicks, fillRests,
    view, playEvents, snapshotOps, digest, toFile, opsFromFile,
  };
});
