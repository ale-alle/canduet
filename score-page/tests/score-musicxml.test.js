// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios
// MusicXML export. (Checked against the MusicXML 4.0 schema and read back with music21 when it was
// written; these tests keep the structure from slipping.)
// Run: node --test score-page/tests/score-musicxml.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const S = require("../score-model.js");

function score(clef) {
  const st = S.emptyState();
  const me = S.createSession(st, "Ms Lee");
  const m1 = me.addMeasure(S.posBetween("", ""), [3, 4], -1);
  const m2 = me.addMeasure(S.posBetween(m1.pos, ""), null, null);
  for (const op of [m1, m2, me.setMeta({ clef, title: "A & B" })]) S.applyOp(st, op);
  const n = (m, t, d, p, more = {}) => S.applyOp(st, me.setNote(null, { m: m.id, t, d, p, tie: false, ...more }));
  n(m1, 0, 480, [[0, 5, 0]]);
  n(m1, 480, 240, [[3, 4, 1]], { st: true });
  n(m1, 720, 240, [[3, 4, 0]]);
  n(m1, 960, 480, [[6, 4, -1]], { tie: true });
  n(m2, 0, 480, [[6, 4, -1]]);
  for (const t of [480, 640, 800]) n(m2, t, 160, [[5, 4, 0]]);
  if (clef === "piano") n(m1, 0, 1440, [[3, 2, 0], [0, 3, 0]], { s: 1 });
  S.applyOp(st, me.addSlur([m1.id, 0], [m1.id, 960], 0));
  return st;
}
const count = (xml, s) => xml.split(s).length - 1;

test("one staff: header, key, time, clef, and every note", () => {
  const xml = S.toMusicXML(score("treble"), { tempo: 90, date: "2026-11-17" });
  assert.ok(xml.startsWith('<?xml version="1.0" encoding="UTF-8" standalone="no"?>'));
  assert.ok(xml.includes('<score-partwise version="4.0">'));
  assert.ok(xml.includes("<work-title>A &amp; B</work-title>"));
  assert.ok(xml.includes("<key><fifths>-1</fifths></key>"));
  assert.ok(xml.includes("<time><beats>3</beats><beat-type>4</beat-type></time>"));
  assert.ok(xml.includes("<clef><sign>G</sign><line>2</line></clef>"));
  assert.ok(xml.includes("<per-minute>90</per-minute>"));
  assert.equal(count(xml, "<measure "), 2);
  assert.ok(!xml.includes("<staves>"));
  // F# shows a sharp, the F after it a natural; the B-flat is in the key (no sign)
  assert.ok(/<step>F<\/step><alter>1<\/alter>.*<accidental>sharp<\/accidental>/.test(xml));
  assert.ok(/<step>F<\/step><octave>4<\/octave>.*<accidental>natural<\/accidental>/.test(xml));
  assert.equal(count(xml, "<accidental>flat</accidental>"), 0);
  // tie over the bar line, staccato, slur, triplets, the final bar line
  assert.equal(count(xml, '<tie type="start"/>'), 1);
  assert.equal(count(xml, '<tie type="stop"/>'), 1);
  assert.equal(count(xml, "<staccato/>"), 1);
  assert.equal(count(xml, '<slur type="start"'), 1);
  assert.equal(count(xml, '<slur type="stop"'), 1);
  assert.equal(count(xml, "<actual-notes>3</actual-notes>"), 3);
  assert.equal(count(xml, '<tuplet type="start"'), 1);
  assert.ok(xml.includes("<bar-style>light-heavy</bar-style>"));
  // every bar adds up: 3 quarters at the divisions used
  const div = +xml.match(/<divisions>(\d+)<\/divisions>/)[1];
  for (const bar of xml.split("<measure ").slice(1)) {
    let sum = 0;
    for (const nm of bar.matchAll(/<note>(<chord\/>)?.*?<duration>(\d+)<\/duration>/g)) if (!nm[1]) sum += +nm[2];
    assert.equal(sum, div * 3);
  }
});

test("piano: one part with two staves, the left hand after a backup", () => {
  const xml = S.toMusicXML(score("piano"));
  assert.ok(xml.includes("<staves>2</staves>"));
  assert.ok(xml.includes('<clef number="1"><sign>G</sign><line>2</line></clef>'));
  assert.ok(xml.includes('<clef number="2"><sign>F</sign><line>4</line></clef>'));
  assert.equal(count(xml, "<backup>"), 2);
  assert.ok(xml.includes("<staff>2</staff>"));
  assert.ok(xml.includes("<voice>5</voice>"));
  assert.ok(xml.includes("<chord/>"));
  assert.ok(xml.includes("<part-name>Piano</part-name>"));
});
