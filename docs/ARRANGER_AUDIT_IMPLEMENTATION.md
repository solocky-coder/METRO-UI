# Arranger Audit Implementation Plan

## Purpose

This document converts the arranger audit into an implementation sequence for the shared sequencer model and both arranger UIs.

The work is deliberately split into three independent phases:

1. **Fix the confirmed `openRecNotes` data race immediately.**
2. **Remove `MidiClip` locking from the audio-thread playback path.**
3. **Consolidate duplicate MIDI-note edit and undo implementations.**

Each phase should be submitted and tested separately. This keeps the urgent correctness fix small, prevents the real-time refactor from being obscured by UI changes, and makes regressions easier to isolate.

---

## Current architecture to preserve

The following existing patterns are sound and should remain intact:

- `SequencerEngine::Impl::currentTracks` publishes immutable track-list snapshots atomically.
- `SequencerTrack` uses the same copy-on-write snapshot model for clip lists.
- Recorded MIDI crosses from the audio thread to the message thread through `juce::AbstractFifo` and POD events.
- `activeNotes` remains audio-thread-owned.
- Project serialization retains its current version checks, bounds validation, and backward compatibility.

No phase below should move heap allocation, UI work, clip mutation, blocking synchronization, or undo processing onto the audio thread.

---

# Phase 1 — Eliminate the `openRecNotes` race

## Priority

**Critical / implement first.** This is a reachable correctness bug that can corrupt `juce::Array<OpenRecNote>` when transport commands overlap `drainRecordedEvents()`.

## Affected code

- `src/sequencer/SequencerEngine.cpp`

## Problem

`openRecNotes` is documented and otherwise used as message-thread-only state. However, `processBlock()` currently calls `openRecNotes.clearQuick()` while handling `pendingStop` and `pendingRewind`. At the same time, `drainRecordedEvents()` may call `size()`, `getReference()`, `add()`, and `remove()` on the same array.

The audio thread must never access `openRecNotes` directly.

## Implementation

### 1. Represent discard as a FIFO control event

Extend the existing recorded-event payload so it can represent both note data and a control command:

```cpp
enum class RecordedEventType : uint8_t
{
    noteOn,
    noteOff,
    discardOpenNotes
};

struct RecordedNoteEvent
{
    RecordedEventType type = RecordedEventType::noteOn;
    int trackIndex = -1;
    int note = 60;
    int velocity = 100;
    int64_t localTick = 0;
};
```

Using the existing single-producer/single-consumer FIFO preserves ordering between notes captured before a transport boundary, the discard command, and notes captured afterward. This is preferable to directly clearing message-thread state or using an unordered shared flag.

Add an audio-thread-only helper:

```cpp
void pushDiscardOpenNotesEvent()
{
    RecordedNoteEvent ev;
    ev.type = RecordedEventType::discardOpenNotes;
    pushRecordedEvent (ev);
}
```

### 2. Replace audio-thread array access

In the `pendingStop` and `pendingRewind` handlers inside `processBlock()`:

- Keep `flushAllActiveNotes()` on the audio thread.
- Remove every direct `openRecNotes.clearQuick()` call.
- Push one `discardOpenNotes` control event instead.

A stop request should enqueue the discard whenever it is consumed, not only when `playing` happens to be true. This makes repeated stop commands harmless and avoids stale recording state.

### 3. Consume the command on the message thread

In `drainRecordedEvents()`, process events in FIFO order:

- `discardOpenNotes` → call `openRecNotes.clearQuick()` and return from the event handler.
- `noteOn` → add the note and track it in `openRecNotes`.
- `noteOff` → locate the matching open note and set its duration as today.

`setRecordingTrack()` may continue clearing `openRecNotes` directly only if its message-thread-only contract is explicit and enforced. Add `jassert (juce::MessageManager::getInstance()->isThisTheMessageThread())` in debug builds, or route this path through a message-thread helper shared with the FIFO consumer.

### 4. Handle FIFO saturation

A discarded transport-control event could leave stale open-note state. Therefore:

- Add a dedicated diagnostic counter for dropped control events, or extend the existing diagnostic to distinguish note loss from control loss.
- If reserving a FIFO slot for control events is impractical, use a message-thread-consumed atomic fallback flag only when the control event cannot be written.
- The fallback must be treated as recovery for overflow, not the normal path, because it does not retain FIFO ordering.

## Phase 1 acceptance criteria

- `processBlock()` never reads, writes, clears, or otherwise references `openRecNotes`.
- All normal `openRecNotes` mutation occurs on the message thread.
- Stop and rewind discard held recording-note bookkeeping without corrupting the array.
- No lock, allocation, or message-thread callback is added to the audio thread.
- Note-on/note-off duration behavior, including loop-wrapped durations, remains unchanged.

## Phase 1 validation

### Automated

Add focused tests around a small extracted recorded-event drain routine if the current test target cannot instantiate the full engine:

1. note-on → note-off sets the expected duration;
2. note-on → discard → note-off leaves no open note and performs no invalid edit;
3. note-on → discard → note-on → note-off affects only the second note;
4. repeated discard commands are idempotent;
5. loop-wrapped note duration remains positive and bounded by clip length;
6. FIFO wrap-around preserves command ordering.

Run ThreadSanitizer where supported and repeatedly issue stop/rewind while the message thread drains recorded input.

### Manual

- Record held notes while rapidly pressing Stop and Rewind.
- Repeat while loop playback is enabled and the playhead crosses the loop boundary.
- Change the armed recording track while notes are held.
- Confirm there are no crashes, stuck notes, unexpectedly resized notes, or stale note-off matches.

---

# Phase 2 — Make `MidiClip` note reads lock-free

## Priority

**High / real-time safety.** This removes the blocking `juce::ReadWriteLock` currently taken by `processClipSlot()` once per clip per audio block.

## Affected code

- `src/sequencer/MidiClip.h`
- `src/sequencer/MidiClip.cpp`
- `src/sequencer/SequencerEngine.cpp`
- Call sites that hold `MidiClip::getLock()` or retain references returned by `getNotes()`

## Target model

Introduce an immutable note snapshot:

```cpp
using NoteList = std::vector<MidiNote>;
std::atomic<std::shared_ptr<const NoteList>> notesSnapshot;
```

Readers acquire one `shared_ptr` and keep it alive for the complete iteration. Writers copy the current list, mutate and sort the copy, then publish it with release semantics.

## API direction

Prefer an API that makes snapshot lifetime explicit:

```cpp
std::shared_ptr<const NoteList> getNotesSnapshot() const noexcept;
```

Avoid returning an unowned reference to snapshot contents. A reference can outlive the temporary `shared_ptr` and become dangling after the next publish.

Every compound edit must publish once. For example, moving several selected notes should clone once, apply all changes, sort once, and atomically publish once rather than cloning per note.

## Migration steps

1. Inventory all uses of `getLock()`, `getNotes()`, note indices, and mutable note references.
2. Add the snapshot storage and safe read API without changing serialization format.
3. Convert `processClipSlot()` to load one snapshot and iterate it without a lock.
4. Convert each mutation (`addNote`, `removeNote`, move, resize, velocity, replace-all, sort, deserialization) to copy–mutate–publish.
5. Add batch-edit methods needed by undo actions and multi-note UI operations.
6. Remove `juce::ReadWriteLock` only after all readers and writers have migrated.
7. Update the file-level threading comment so the lock-free claim precisely matches the implementation.

## Index stability requirement

Current recording code stores `noteIndexInClip` in `OpenRecNote`. Copy-on-write does not itself invalidate an integer index, but unrelated edits or sorting can change what that index identifies before note-off arrives.

Before completing Phase 2, replace index identity with a stable note identifier or another validated lookup strategy. Recommended approach:

- assign each `MidiNote` an internal stable ID that is not serialized unless project persistence requires it;
- store that ID in `OpenRecNote`;
- update note duration by ID;
- generate fresh IDs after loading legacy project data.

This prevents recording finalization from resizing the wrong note while the user edits the same clip.

## Phase 2 acceptance criteria

- The audio thread takes no `MidiClip` lock.
- Audio-thread note iteration performs one atomic snapshot load per clip and no allocation.
- Writers publish complete, sorted snapshots atomically.
- Multi-note edits publish once per logical edit.
- Serialization remains backward compatible and byte validation remains intact.
- Recording finalizes notes using stable identity rather than a potentially stale array index.

## Phase 2 validation

- Stress playback while pasting, deleting, moving, resizing, quantizing, undoing, and loading large clips.
- Compare MIDI output before and after the refactor using a deterministic clip fixture.
- Verify readers see either the complete old state or complete new state, never a partial edit.
- Profile or instrument the audio callback to confirm no lock acquisition and no new allocation.
- Run ThreadSanitizer over concurrent snapshot reads and message-thread edits.

---

# Phase 3 — Consolidate MIDI edit and undo behavior

## Priority

**Medium / maintainability and consistency.** Begin only after Phases 1 and 2 are stable, because the shared actions should target the final `MidiClip` editing API.

## Affected code

- New shared action module, for example:
  - `src/sequencer/MidiClipUndoActions.h`
  - `src/sequencer/MidiClipUndoActions.cpp` if non-header-only
- `src/ui/PianoRollComponent.h`
- `src/metro/MetroPianoRoll.cpp`
- `src/metro/MetroStepSequencer.cpp`
- Corresponding headers and build configuration if a new `.cpp` file is added

## Shared actions

Extract reusable `juce::UndoableAction` implementations for at least:

- add/delete or replace clip-note state;
- move notes;
- resize notes;
- set velocity;
- batch edits affecting a selected set.

Actions should contain model operations and before/after data only. Grid snapping, mouse gestures, selection painting, and visual feedback remain UI responsibilities.

## Transaction rules

Define consistent transaction boundaries across all editors:

- one mouse drag = one undo step;
- one velocity paint gesture = one undo step;
- one paste/delete/quantize command = one undo step;
- redo reproduces the exact post-edit note state;
- switching clips or destroying an editor cannot leave an action with a dangling raw clip pointer.

Use a safe clip lifetime strategy, such as a stable owner handle plus validation, rather than assuming every `MidiClip*` remains valid for the lifetime of the undo history.

## Embedded piano-roll migration

Replace `PianoRollComponent.h`’s full-array snapshot stacks with `juce::UndoManager` and the shared actions. Preserve existing keyboard shortcuts and user-visible behavior.

Where an edit legitimately replaces the whole note list, one before/after snapshot action is acceptable. Routine single-note and small batch edits should not copy and retain the entire undo history unnecessarily beyond what the Phase 2 snapshot model already requires.

## Metro migration

Remove the local action classes from `MetroPianoRoll.cpp` and `MetroStepSequencer.cpp`, then call the shared action factories/helpers. Delete comments claiming one implementation mirrors another; there should be only one source of edit semantics.

## Phase 3 acceptance criteria

- Shared note-edit behavior has one implementation.
- All three editors use `juce::UndoManager` consistently.
- Gesture transaction boundaries match across editors.
- Undo/redo preserves note position, duration, pitch, velocity, selection-relevant identity, sorting, and loop-boundary rules.
- No undo action holds an unsafe dangling model pointer.
- Existing shortcuts and editor interactions remain functional.

## Phase 3 validation matrix

Run each operation in the embedded piano roll, Metro piano roll, and Metro step sequencer:

- add and delete;
- single and multi-note move;
- resize from both applicable edges;
- velocity change/paint;
- paste and duplicate;
- edits at tick zero and the clip end;
- edits that cross or wrap a loop boundary;
- undo repeatedly to the initial state, then redo to the final state;
- switch/remove clips with populated undo history.

The same logical operation must produce equivalent clip data in every editor.

---

# Delivery sequence

Use three reviewable pull requests:

1. **`fix/sequencer-open-record-note-race`**  
   Phase 1 only, including focused tests and updated threading comments.

2. **`refactor/midiclip-note-snapshots`**  
   Phase 2 only, including stable note identity, migrated callers, stress tests, and performance validation.

3. **`refactor/shared-midi-undo-actions`**  
   Phase 3 only, including all editor migrations and the cross-editor behavior matrix.

Do not combine these phases unless repository constraints make separate reviews impossible.

---

# Definition of done

The arranger work is complete when:

- `openRecNotes` is message-thread-owned with no audio-thread access;
- transport discard requests cross threads without blocking and retain event ordering;
- playback reads immutable MIDI-note snapshots without taking a lock;
- note edits publish complete states atomically;
- recording uses stable note identity;
- all arranger editors share one undo/action implementation;
- serialization compatibility is preserved;
- automated and manual stress tests pass without races, stuck notes, corrupted clips, or audio-thread blocking.
