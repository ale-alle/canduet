// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
// Grand staff (piano): each staff its own rhythm; slur shapes.
// Run: node --test score-page/tests/score-staves.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const S = require("../score-model.js");

test("piano: two staves, each with its own rhythm and rests", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  S.applyOp(st, me.setMeta({ clef: "piano" }));
  // right hand: four quarters; left hand: a whole note at the same time
  for (const t of [0, 480, 960, 1440]) S.applyOp(st, me.setNote(null, { m: m.id, t, d: 480, p: [[0, 5, 0]], tie: false }));
  S.applyOp(st, me.setNote(null, { m: m.id, t: 0, d: 1920, p: [[0, 3, 0], [4, 3, 0]], tie: false, s: 1 }));
  const v = S.view(st).measures[0];
  assert.equal(v.staves.length, 2);
  assert.deepEqual(v.staves[0].items.map((i) => (i.rest ? "r" : "n") + i.d), ["n480", "n480", "n480", "n480"]);
  assert.deepEqual(v.staves[1].items.map((i) => (i.rest ? "r" : "n") + i.d), ["n1920"]);
  assert.equal(v.hidden, 0);
  // both play
  const ev = S.playEvents(S.view(st));
  assert.equal(ev.length, 6);
  // one staff again: everything on it, the overlaps hidden (nothing is lost)
  S.applyOp(st, me.setMeta({ clef: "treble" }));
  const v1 = S.view(st).measures[0];
  assert.equal(v1.staves.length, 1);
  assert.equal(v1.hidden, 4);
  S.applyOp(st, me.setMeta({ clef: "piano" }));
  assert.equal(S.view(st).measures[0].hidden, 0);
});

test("lower-staff notes and slurs: saved, shared, refused when bad", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  S.applyOp(st, me.setMeta({ clef: "piano" }));
  for (const t of [0, 480, 960]) S.applyOp(st, me.setNote(null, { m: m.id, t, d: 480, p: [[0, 3, 0]], tie: false, s: 1 }));
  const sl = me.addSlur([m.id, 0, 1], [m.id, 960, 1], 0, { side: -1, o: [[m.id, 480, -6]] });
  assert.equal(S.validateOp(sl), null);
  S.applyOp(st, sl);
  const v = S.view(st);
  assert.deepEqual(v.slurs[0].a, { mi: 0, t: 0, s: 1 });
  assert.equal(v.slurs[0].side, -1);
  assert.deepEqual(v.slurs[0].o, [{ mi: 0, t: 480, dy: -6 }]);
  // only the lower staff's notes are slurred
  const ev = S.playEvents(v);
  assert.ok(ev.every((e) => e.legato));
  // a snapshot and a file keep all of it
  const b = S.emptyState();
  for (const op of S.snapshotOps(st)) S.applyOp(b, op);
  assert.equal(S.digest(b), S.digest(st));
  const file = JSON.parse(JSON.stringify(S.toFile(st)));
  const c = S.emptyState();
  for (const op of S.opsFromFile(S.createSession(c, "Zoe"), file)) S.applyOp(c, op);
  const vc = S.view(c);
  assert.equal(vc.meta.clef, "piano");
  assert.equal(vc.measures[0].staves[1].items.filter((i) => !i.rest).length, 3);
  assert.deepEqual(vc.slurs.map((x) => [x.a.s, x.side, x.o.length]), [[1, -1, 1]]);
  // moving a point, flipping the side
  S.applyOp(st, me.setSlur(sl.id, { side: 1, o: [[m.id, 480, 10], [m.id, 960, -4]] }));
  assert.equal(S.view(st).slurs[0].side, 1);
  // bad values
  const bad = [
    { ...sl, id: "s.x", slur: { ...sl.slur, side: 2 } },
    { ...sl, id: "s.x", slur: { ...sl.slur, o: [[m.id, 480, 999]] } },
    { ...sl, id: "s.x", slur: { ...sl.slur, a: [m.id, 0, 2] } },
    { ...sl, id: "s.x", slur: { ...sl.slur, extra: 1 } },
    me.setNote(null, { m: m.id, t: 0, d: 480, p: [[0, 3, 0]], tie: false, s: 0 }),
    me.setNote(null, { m: m.id, t: 0, d: 480, p: [[0, 3, 0]], tie: false, x: 1 }),
  ];
  for (const op of bad) assert.notEqual(S.validateOp(op), null, JSON.stringify(op));
});
