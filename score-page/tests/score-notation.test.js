// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
// How the score is written down: rests per time signature, tied pieces, triplets.
// Run: node --test score-page/tests/score-notation.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const S = require("../score-model.js");

test("rests follow the time signature", () => {
  // 6/8: a silent beat is a dotted quarter rest; part of a beat is eighths
  assert.deepEqual(S.fillRests(0, 1440, [6, 8]), [{ t: 0, d: 720 }, { t: 720, d: 720 }]);
  assert.deepEqual(S.fillRests(240, 1440, [6, 8]), [{ t: 240, d: 240 }, { t: 480, d: 240 }, { t: 720, d: 720 }]);
  assert.deepEqual(S.fillRests(840, 1440, [6, 8]), [{ t: 840, d: 120 }, { t: 960, d: 240 }, { t: 1200, d: 240 }]);
  // 4/4: a half rest only on beat 1 or 3; never across the middle of the bar
  assert.deepEqual(S.fillRests(480, 1440, [4, 4]), [{ t: 480, d: 480 }, { t: 960, d: 480 }]);
  assert.deepEqual(S.fillRests(960, 1920, [4, 4]), [{ t: 960, d: 960 }]);
  // 5/8: nothing longer than a quarter rest
  assert.deepEqual(S.fillRests(0, 1200, [5, 8]), [{ t: 0, d: 480 }, { t: 480, d: 480 }, { t: 960, d: 240 }]);
  // 3/4: beats 2-3 are two quarter rests
  assert.deepEqual(S.fillRests(480, 1440, [3, 4]), [{ t: 480, d: 480 }, { t: 960, d: 480 }]);
});

test("a note too long for the bar is written as tied pieces", () => {
  // a half note on beat 4 of 4/4: a quarter here, a quarter in the next bar
  assert.deepEqual(S.notePieces(1440, 960, 1920, [4, 4]), { pieces: [{ t: 1440, d: 480 }], left: 480 });
  // a whole note from beat 2: dotted half here (one written length), a quarter left
  assert.deepEqual(S.notePieces(480, 1920, 1920, [4, 4]), { pieces: [{ t: 480, d: 1440 }], left: 480 });
  // 5 eighths from beat 1 in 4/4: half + eighth, tied, on the beat
  assert.deepEqual(S.notePieces(0, 1200, 1920, [4, 4]), { pieces: [{ t: 0, d: 960 }, { t: 960, d: 240 }], left: 0 });
  // fits: one piece
  assert.deepEqual(S.notePieces(0, 720, 1440, [6, 8]), { pieces: [{ t: 0, d: 720 }], left: 0 });
});

test("where triplet groups start", () => {
  assert.equal(S.tripletStart(0, 160), 0);
  assert.equal(S.tripletStart(320, 160), 0);
  assert.equal(S.tripletStart(480, 160), 480);
  assert.equal(S.tripletStart(200, 160), null);       // not on a triplet-8th grid
  assert.equal(S.tripletStart(480, 320), 480);        // triplet quarters can start on beat 2
  assert.equal(S.tripletStart(800, 320), 480);
  assert.equal(S.tripletStart(640, 320), 0);
  assert.equal(S.tripletStart(240, 320), null);
  // drawn as a group from beat 2: rest, then a group of three, then the rest of the bar
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  for (const t of [480, 800, 1120]) S.applyOp(st, me.setNote(null, { m: m.id, t, d: 320, p: [[0, 5, 0]], tie: false }));
  const v = S.view(st).measures[0];
  assert.deepEqual(v.tuplets, [{ start: 480, len: 960 }]);
  assert.deepEqual(v.items.map((i) => (i.rest ? "r" : "n") + i.d), ["r480", "n320", "n320", "n320", "r480"]);
});

test("triplets: groups of three, with triplet rests in the gaps", () => {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m = me.addMeasure(S.posBetween("", ""), [4, 4], 0);
  S.applyOp(st, m);
  // beat 1: three triplet eighths; beat 3: the first and last triplet eighth only
  for (const t of [0, 160, 320]) S.applyOp(st, me.setNote(null, { m: m.id, t, d: 160, p: [[0, 5, 0]], tie: false }));
  S.applyOp(st, me.setNote(null, { m: m.id, t: 960, d: 160, p: [[2, 5, 0]], tie: false }));
  S.applyOp(st, me.setNote(null, { m: m.id, t: 1280, d: 160, p: [[4, 5, 0]], tie: false }));
  const v = S.view(st).measures[0];
  assert.deepEqual(v.tuplets, [{ start: 0, len: 480 }, { start: 960, len: 480 }]);
  const desc = v.items.map((i) => (i.rest ? "r" : "n") + i.d + (i.tup !== undefined ? "3" : ""));
  assert.deepEqual(desc, ["n1603", "n1603", "n1603", "r480", "n1603", "r1603", "n1603", "r480"]);
  // they play a third of a beat apart
  const ev = S.playEvents(S.view(st));
  assert.deepEqual(ev.map((e) => +e.beat.toFixed(4)), [0, 0.3333, 0.6667, 2, 2.6667]);
  // triplet quarters make a half-note group
  const m2 = me.addMeasure(S.posBetween(m.pos, ""), null, null);
  S.applyOp(st, m2);
  S.applyOp(st, me.setNote(null, { m: m2.id, t: 320, d: 320, p: [[0, 5, 0]], tie: false }));
  const v2 = S.view(st).measures[1];
  assert.deepEqual(v2.tuplets, [{ start: 0, len: 960 }]);
  assert.deepEqual(v2.items.map((i) => (i.rest ? "r" : "n") + i.d), ["r320", "n320", "r320", "r960"]);
});
