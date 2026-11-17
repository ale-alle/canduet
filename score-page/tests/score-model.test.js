// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
// Run: node --test score-page/tests/
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const S = require("../score-model.js");

const C4 = [0, 4, 0], D4 = [1, 4, 0], E4 = [2, 4, 0], G4 = [4, 4, 0], Bb3 = [6, 3, -1];
const note = (m, t, d, p, tie = false) => ({ m, t, d, p, tie });

function pair() {
  const a = S.emptyState(), b = S.emptyState();
  return { a, b, ann: S.createSession(a, "Ms Lee"), bob: S.createSession(b, "Ava") };
}
const apply = (states, ops) => { for (const st of states) for (const op of [].concat(ops)) S.applyOp(st, op); };
const shuffled = (arr, seed) => {
  const out = arr.slice();
  let s = seed;
  for (let i = out.length - 1; i > 0; i--) { s = (s * 1103515245 + 12345) & 0x7fffffff; const j = s % (i + 1); [out[i], out[j]] = [out[j], out[i]]; }
  return out;
};

test("pitches: MIDI numbers and spelling for the key", () => {
  assert.equal(S.midiOf(C4), 60);
  assert.equal(S.midiOf(Bb3), 58);
  assert.deepEqual(S.spell(61, 0), [0, 4, 1]);    // C#4 in C
  assert.deepEqual(S.spell(61, -2), [1, 4, -1]);  // Db4 in Bb
  assert.deepEqual(S.spell(59, 0), [6, 3, 0]);    // B3
  assert.deepEqual(S.spell(48, 0), [0, 3, 0]);    // C3
  assert.equal(S.keyAlter(2, 3), 1);   // D major: F#
  assert.equal(S.keyAlter(2, 4), 0);   // but G natural
  assert.equal(S.keyAlter(-1, 6), -1); // F major: Bb
});

test("positions sort between their neighbours", () => {
  let keys = [S.posBetween("", "")];
  for (let i = 0; i < 200; i++) {
    const at = (i * 7) % (keys.length + 1);
    const k = S.posBetween(at === 0 ? "" : keys[at - 1], at === keys.length ? "" : keys[at]);
    if (at > 0) assert.ok(keys[at - 1] < k, `${keys[at - 1]} < ${k}`);
    if (at < keys.length) assert.ok(k < keys[at], `${k} < ${keys[at]}`);
    keys.splice(at, 0, k);
  }
  assert.deepEqual(keys.slice().sort(), keys);
});

test("rests fill the gaps on the beat", () => {
  assert.deepEqual(S.fillRests(240, 960), [{ t: 240, d: 240 }, { t: 480, d: 480 }]);
  assert.deepEqual(S.fillRests(0, 1920), [{ t: 0, d: 1920 }]);
  assert.deepEqual(S.fillRests(0, 1440), [{ t: 0, d: 960 }, { t: 960, d: 480 }]);
  assert.deepEqual(S.fillRests(1200, 1440), [{ t: 1200, d: 240 }]);
});

test("a measure shows notes and the rests between them", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  S.applyOp(st, me.setNote(null, note(m.id, 480, 480, [C4])));
  S.applyOp(st, me.setNote(null, note(m.id, 1440, 240, [E4, G4])));
  const v = S.view(st);
  const items = v.measures[0].items.map((i) => (i.rest ? `r${i.d}` : `n${i.d}`));
  assert.deepEqual(items, ["r480", "n480", "r480", "n240", "r240"]);
  const empty = S.createSession(st, "x").addMeasure(S.posBetween(m.pos, ""), null, null);
  S.applyOp(st, empty);
  const v2 = S.view(st);
  assert.equal(v2.measures[1].items[0].whole, true);
  assert.equal(v2.measures[1].beat, 4);
});

test("time and key carry on until changed", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  let pos = "";
  const ms = [];
  for (const [time, key] of [[[3, 4], 2], [null, null], [[6, 8], null], [null, -1]]) {
    pos = S.posBetween(pos, "");
    const op = me.addMeasure(pos, time, key);
    S.applyOp(st, op);
    ms.push(op.id);
  }
  const v = S.view(st);
  assert.deepEqual(v.measures.map((m) => m.time.join("/")), ["3/4", "3/4", "6/8", "6/8"]);
  assert.deepEqual(v.measures.map((m) => m.key), [2, 2, 2, -1]);
  assert.deepEqual(v.measures.map((m) => m.timeChanged), [true, false, true, false]);
  assert.deepEqual(v.measures.map((m) => m.keyChanged), [true, false, false, true]);
  assert.deepEqual(v.measures.map((m) => m.beat), [0, 3, 6, 9]);
  assert.equal(v.totalBeats, 12);
});

test("both people's edits end up the same whatever the order", () => {
  for (let seed = 1; seed <= 30; seed++) {
    const { a, b, ann, bob } = pair();
    const m1 = ann.addMeasure(S.posBetween("", ""), [4, 4], 0);
    apply([a, b], m1);
    const ops = [];
    // both write at the same time, into the same and different places
    const n1 = ann.setNote(null, note(m1.id, 0, 960, [C4])); S.applyOp(a, n1); ops.push(n1);
    const n2 = bob.setNote(null, note(m1.id, 480, 480, [D4])); S.applyOp(b, n2); ops.push(n2);  // overlaps n1
    const n3 = bob.setNote(null, note(m1.id, 960, 480, [E4])); S.applyOp(b, n3); ops.push(n3);
    const k = bob.setMeasure(m1.id, { key: 1 }); S.applyOp(b, k); ops.push(k);
    const k2 = ann.setMeasure(m1.id, { key: -3 }); S.applyOp(a, k2); ops.push(k2);
    const t = ann.setMeta({ clef: "bass" }); S.applyOp(a, t); ops.push(t);
    const m2 = bob.addMeasure(S.posBetween(m1.pos, ""), null, null); S.applyOp(b, m2); ops.push(m2);
    const m3 = ann.addMeasure(S.posBetween(m1.pos, ""), null, null); S.applyOp(a, m3); ops.push(m3);  // same spot
    const r = ann.removeNote(n3.id); S.applyOp(a, r); ops.push(r);  // removed before ann even had it
    // deliver everything to both in different orders, some twice
    const x = S.emptyState(), y = S.emptyState();
    for (const op of shuffled(ops.concat(ops.slice(0, 3)), seed)) { S.applyOp(a, op); S.applyOp(x, op); }
    for (const op of shuffled(ops, seed * 7 + 3)) { S.applyOp(b, op); S.applyOp(y, op); }
    S.applyOp(x, m1); S.applyOp(y, m1);
    for (const op of ops) { S.applyOp(x, op); S.applyOp(y, op); }  // m1 arrived late there: replay
    assert.equal(S.digest(a), S.digest(b), `seed ${seed}`);
    assert.equal(S.digest(x), S.digest(a), `seed ${seed}`);
    assert.deepEqual(S.view(a), S.view(b));
    const v = S.view(a);
    assert.equal(v.measures.length, 3);
    assert.equal(v.meta.clef, "bass");
    assert.ok(!v.measures.some((m) => m.items.some((i) => i.id === n3.id)), "removed note stays removed");
    // the later of the two overlapping notes is shown, the other hidden
    assert.equal(v.measures[0].hidden, 1);
  }
});

test("removed things can't come back", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  const n = me.setNote(null, note(m.id, 0, 480, [C4]));
  apply([st], [m, n]);
  S.applyOp(st, me.removeNote(n.id));
  assert.equal(S.applyOp(st, n), false);
  assert.equal(S.applyOp(st, { ...n, ts: n.ts + 100 }), false);
  S.applyOp(st, me.removeMeasure(m.id));
  assert.equal(S.applyOp(st, m), false);
  assert.equal(S.applyOp(st, me.setNote(null, note(m.id, 0, 480, [D4]))), false, "no notes in a removed measure");
  assert.equal(S.view(st).measures.length, 0);
});

test("a note can be changed (last writer wins)", () => {
  const { a, b, ann, bob } = pair();
  const m = ann.addMeasure(S.posBetween("", ""), [4, 4], 0);
  const n = ann.setNote(null, note(m.id, 0, 480, [C4]));
  apply([a, b], [m, n]);
  const e1 = ann.setNote(n.id, note(m.id, 0, 960, [C4]));
  const e2 = bob.setNote(n.id, note(m.id, 0, 480, [G4]));
  S.applyOp(a, e1); S.applyOp(b, e2); S.applyOp(a, e2); S.applyOp(b, e1);
  assert.equal(S.digest(a), S.digest(b));
  assert.equal(a.notes[n.id].d, e1.ts > e2.ts || (e1.ts === e2.ts && "Ms Lee" > "Ava") ? 960 : 480);
});

test("reset clears what came before it, not what came after", () => {
  const { a, b, ann, bob } = pair();
  const m = ann.addMeasure(S.posBetween("", ""), [4, 4], 0);
  apply([a, b], m);
  const r = ann.reset();
  S.applyOp(a, r);
  const late = bob.addMeasure(S.posBetween("", ""), [3, 4], 0);
  late.ts = r.ts + 5;
  apply([a, b], late);
  S.applyOp(b, r);
  assert.equal(S.digest(a), S.digest(b));
  assert.deepEqual(S.view(a).measures.map((x) => x.id), [late.id]);
  assert.equal(S.applyOp(a, m), false, "an old add after the reset is ignored");
});

test("a whole score resent as a snapshot repairs a copy that missed messages", () => {
  const { a, ann } = pair();
  let pos = "";
  for (let i = 0; i < 8; i++) {
    pos = S.posBetween(pos, "");
    const m = ann.addMeasure(pos, i === 0 ? [3, 4] : null, i === 0 ? -2 : null);
    S.applyOp(a, m);
    for (let t = 0; t < 1440; t += 480) S.applyOp(a, ann.setNote(null, note(m.id, t, 480, [S.spell(60 + i + t / 480, -2)])));
  }
  const some = Object.keys(a.notes).slice(0, 4);
  for (const id of some) S.applyOp(a, ann.removeNote(id));
  S.applyOp(a, ann.setMeasure(Object.keys(a.measures)[2], { time: [2, 4] }));
  S.applyOp(a, ann.setMeta({ title: "Etude", instrument: "guitar" }));

  const b = S.emptyState();
  // b got a few of the notes that a later removed, from an older message
  const stale = S.emptyState();
  const bob = S.createSession(stale, "Ava");
  void bob;
  const snap = S.snapshotOps(a);
  for (const op of snap) assert.equal(S.validateOp(op), null, JSON.stringify(op));
  for (const op of shuffled(snap, 5)) S.applyOp(b, op);
  assert.equal(S.digest(b), S.digest(a));
  assert.deepEqual(S.view(b), S.view(a));
  // applying it again changes nothing
  for (const op of snap) S.applyOp(b, op);
  assert.equal(S.digest(b), S.digest(a));
});

test("bad or hostile operations are refused", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  const good = me.setNote(null, note(m.id, 0, 480, [C4]));
  const bad = [
    null, 5, "x", [],
    { ...good, v: 2 },
    { ...good, type: "explode" },
    { ...good, extra: 1 },
    { ...good, id: "../../x" },
    { ...good, id: "x".repeat(65) },
    { ...good, by: "" },
    { ...good, ts: 0 },
    { ...good, ts: 1.5 },
    { ...good, note: { ...good.note, d: 100 } },
    { ...good, note: { ...good.note, t: 30 } },
    { ...good, note: { ...good.note, p: [] } },
    { ...good, note: { ...good.note, p: [C4, C4] } },
    { ...good, note: { ...good.note, p: [[7, 4, 0]] } },
    { ...good, note: { ...good.note, p: [[0, 10, 0]] } },
    { ...good, note: { ...good.note, p: new Array(11).fill(0).map((_, i) => [i % 7, 3 + Math.floor(i / 7), 0]) } },
    { ...good, note: { ...good.note, x: 1 } },
    { ...good, note: { ...good.note, tie: "yes" } },
    { ...m, id: "m.z", pos: "a/b" },
    { ...m, id: "m.z", time: [4, 3] },
    { ...m, id: "m.z", key: 8 },
    { ...me.setMeta({ clef: "alto" }) },
    { ...me.setMeta({ colour: "red" }) },
    { ...me.setMeasure(m.id, { pos: "0" }) },
  ];
  for (const op of bad) {
    assert.notEqual(S.validateOp(op), null, JSON.stringify(op));
    assert.equal(S.applyOp(st, op), false);
  }
  assert.equal(S.validateOp(good), null);
  assert.equal(Object.keys(st.notes).length, 0);
});

test("limits stop a flood", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  let pos = "";
  for (let i = 0; i < S.LIMITS.measures + 20; i++) { pos = S.posBetween(pos, ""); S.applyOp(st, me.addMeasure(pos, null, null)); }
  assert.equal(Object.keys(st.measures).length, S.LIMITS.measures);
});

test("playback events: ties join, rests are skipped, measures follow on", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m1 = me.addMeasure(S.posBetween("", ""), [3, 4], 0);
  const m2 = me.addMeasure(S.posBetween(m1.pos, ""), null, null);
  apply([st], [m1, m2]);
  S.applyOp(st, me.setNote(null, note(m1.id, 0, 480, [C4, E4])));
  S.applyOp(st, me.setNote(null, note(m1.id, 960, 480, [G4], true)));   // tied over the bar
  S.applyOp(st, me.setNote(null, note(m2.id, 0, 240, [G4])));
  S.applyOp(st, me.setNote(null, note(m2.id, 480, 720, [D4])));
  const ev = S.playEvents(S.view(st));
  assert.deepEqual(ev, [
    { beat: 0, len: 1, midi: 60 },
    { beat: 0, len: 1, midi: 64 },
    { beat: 2, len: 1.5, midi: 67 },
    { beat: 4, len: 1.5, midi: 62 },
  ]);
});

test("files round-trip and bad files are refused", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m1 = me.addMeasure(S.posBetween("", ""), [6, 8], 3);
  apply([st], [m1, me.setMeta({ title: "Jig", clef: "treble" })]);
  S.applyOp(st, me.setNote(null, note(m1.id, 0, 720, [[0, 5, 1]])));
  const file = JSON.parse(JSON.stringify(S.toFile(st)));

  const other = S.emptyState();
  const you = S.createSession(other, "Ava");
  S.applyOp(other, you.addMeasure(S.posBetween("", ""), [4, 4], 0));  // replaced by the file
  const ops = S.opsFromFile(you, file);
  assert.ok(ops);
  for (const op of ops) S.applyOp(other, op);
  const v = S.view(other);
  assert.equal(v.measures.length, 1);
  assert.deepEqual(v.measures[0].time, [6, 8]);
  assert.equal(v.measures[0].key, 3);
  assert.equal(v.meta.title, "Jig");
  assert.deepEqual(v.measures[0].items.filter((i) => !i.rest).map((i) => i.p), [[[0, 5, 1]]]);

  assert.equal(S.opsFromFile(you, { format: "x" }), null);
  assert.equal(S.opsFromFile(you, { ...file, measures: [{ time: [4, 4], key: 0, notes: [{ t: 0, d: 7, p: [C4] }] }] }), null);
  assert.equal(S.opsFromFile(you, { ...file, measures: [] }), null);
});
