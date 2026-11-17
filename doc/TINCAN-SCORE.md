# Shared score (T-080, Phase A)

One staff of music that everyone in the lesson writes in together, played on each computer at the metronome's tempo. Owner's plan and decisions (2026-11-17): Phase A score → B notes/tab toggle and chords with guitar diagrams → C recording from the keyboard to the click → D play-along scoring.

## What you see
- **Score** on the lesson bar (key **S**), in lessons and in Practice mode. It opens the Score window. Closing the window only hides it; the score stays until the lesson or practice ends. **Save it to keep it.**
- When someone else starts writing and your window isn't open, a notice says "*Name* is writing in the score. Press Score to see it."
- **Writing:**
  - Click the staff to write a note at the spot nearest the pointer. Notes snap to the length being written. A ghost note shows where it will go.
  - **Shift+click** adds a note to the chord there. Clicking a note selects it.
  - Keys work like MuseScore:
    - **A–G** write at the blue cursor; **Shift+A–G** adds to the last chord.
    - **3 4 5 6 7** pick 16th, 8th, quarter, half, whole; **.** dots the note; **0** writes a rest.
    - **↑ ↓** move the selected note a semitone (**Ctrl**: an octave). **← →** step through the notes.
    - **+** ties the note to the next one. **Delete** removes it. **Ctrl+Z / Ctrl+Y** undo and redo.
  - **Write from keys** (**N**): notes and chords you play on the MIDI keyboard go in at the cursor. Keys pressed within 60 ms count as one chord.
  - **Rest** mode makes clicks and letters write silence. Gaps between notes always show as rests.
- **Bars:**
  - Clef (treble/bass) for the whole score. Time signature and key from the selected bar onwards.
  - **+ Bar** adds a bar after the selected one; **− Bar** removes the selected bar. Writing past the last bar adds one.
- **Sound** (Piano, Guitar, Bass):
  - Each computer plays its own (owner decision). Nothing goes to the others.
  - Guitar and bass are written an octave above how they sound, as usual. Bass also switches to the bass clef.
- **Tempo:**
  - The box shows the metronome's tempo, and changing it changes the metronome.
  - **Anyone in the lesson can change the tempo, and everyone's metronome follows.** This works from the Metronome tab too.
- **Play** (**Space**):
  - Plays from the selected bar. With **Click** on, the metronome starts with its count-in and the music starts after it. If the metronome is already going, the music starts at the next bar.
  - Playback follows the metronome's tempo as it changes. It stops at the end, and switches the metronome off again if Play switched it on.
- **New**, **Open…**, **Save…** (Ctrl+S): New and Open replace the score for everyone (Undo brings it back). Save writes `<title>.score.json`.

**Not in Phase A:** tab, chord symbols and diagrams (Phase B), recording to the click (C), scoring (D), two staves, triplets, voices, lyrics, dynamics, and saving to an assignment or the school library.

## How it's built
| Part | Where |
|---|---|
| Model | `score-page/score-model.js`. No DOM, CommonJS for tests. |
| Page | `score-page/score.html` and `score-ui.js`. Drawn with VexFlow 4.2.5 (`vexflow-bravura.js`, MIT, unchanged from npm). |
| Window | `Source/TinCanScore.{h,cpp}`: `TinCan::ScoreWindow`, WebView2. The page files are in the app (BinaryData), written to `%APPDATA%\CanDuet\score\`. `TINCAN_SCORE_PAGE=<folder>` loads them from a folder instead. |
| Sound | `Source/TinCanScorePlayer.{h,cpp}`: TinySoundFont (`deps/tsf/tsf.h`, MIT) and GeneralUser GS 2.0.3 (`sounds\GeneralUser-GS.sf2` next to the exe; licence in `deps/tsf/`). |
| Timing, tempo messages | `Source/TinCanScoreCore.{h,cpp}`. No JUCE, tested in `TinCanTests`. |
| Wiring | Editor: `showScore`, `getTempoShare`, `handleScoreMessages`. Processor: `getScorePlayer()`, mixed in after the metronome. Lesson page: Score button, `{"t":"score"}`, `docks.score`. |

**The model:**
- A score is a list of bars, ordered by fractional position keys. Notes are placed by tick in their bar (480 a quarter note).
- Rests are never stored, so two people writing in different places never clash.
- Every change is an operation. Changes win by Lamport time, then by author. Removals leave tombstones, and *reset* clears everything written before it.
- Two notes written over each other at the same moment: the later one shows and the other is hidden. Both sides do the same, so they agree.
- Operations are checked before use, whoever sent them: fields, ranges, size limits (400 bars, 6000 notes, 10 notes per chord).

**Sharing:**
- Uses the lesson's encrypted peer channel, tag `canduet-score-1`.
- Each message is under 3000 bytes. New changes go out every 50 ms.
- Every 2 s each side sends a fingerprint of its copy. If they differ, both send their whole score (the newest 1500 removals). The resend waits longer each time the copies still differ, up to once a minute.
- Applying anything twice is harmless, so lost messages repair themselves. A score window opened late gets the whole score the same way.

**Tempo:**
- Tag `canduet-tempo-1`, `{"bpm","seq"}`. A change here gets the next counter. The highest counter wins, ties go to the higher name. The newest change is repeated every 5 s.

**Playback:**
- The page sends the notes in beats. The audio thread plays them on the metronome's own beat count: after the count-in, or at the next bar line.
- A note belongs to the audio block that holds its nearest sample, the same rule as the metronome's clicks.
- TinySoundFont's voices are made before playing, so the audio thread never allocates. Its output goes to this computer's speakers only, never to the others.

## Tests
- `node --test score-page/tests/score-model.test.js`: 14 tests. Convergence in any order over 30 shuffles, tombstones, last writer wins, reset, repair by snapshot, hostile input, limits, rests, playback events with ties, files.
- `TinCanTests`, new score checks:
  - timing at sample accuracy, the same in any block size
  - a repeated note restarting, and the count-in
  - bad notes skipped, release
  - the start point against the metronome
  - tempo message parsing and the newest-wins rule
- Headless Edge, two copies of the page joined by a test relay that drops 40% of messages: same score afterwards, no script errors.
- In the app (build with `TINCAN_DATA_FOLDER=CanDuet-Score`):
  - Practice: Score button → window, write notes, tempo 150 set in the page → playback measured at 150.1 bpm, stops by itself.
  - MIDI chord entry, by sending the app's notes message.
  - Two copies in one lesson (mock join server): edits on both sides arrive and match, and a tempo change from the learner reaches the teacher's metronome.
- **Needs a person:** listening to the sound, a real MIDI keyboard, the Save dialog, and two homes.

## To ship
- **Website:** copy `video-page/lesson.html` to the site's `public/app/video/` so real lessons show the Score button. Practice already uses the app's copy.
- **Installer:** `release/build-installer.ps1` now copies `sounds\`, about 32 MB more before compression.
- **Sound bank:** CMake downloads it once at configure time from a fixed commit and checks its SHA-256, so it isn't kept in git.
