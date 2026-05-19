# Inkternity — Per-Waypoint Audio Design Doc

> **Audience:** the agent (and any human contributor) working inside the Inkternity repo.
>
> **Goal of this doc:** define a dead-simple per-waypoint audio system — an mp3 file attached to a waypoint plays in reader mode, hard-cuts to a new clip on entering another audio-bearing waypoint, stops on entering a "stop audio" waypoint, and replays from the chain's predecessor when the reader presses Back. Author-mode is silent. No timeline, no mixer, no fades.

## 1. Product summary

A waypoint can carry three new fields:

- **`audioId`** — a `NetObjID` referencing a `ResourceData` blob (mp3 bytes), or `{0,0}` for "no audio."
- **`audioLoops`** — whether the clip restarts from the beginning when it finishes (otherwise it plays once and goes silent).
- **`stopAudio`** — independent flag. When the reader arrives at a stop-audio waypoint, any in-flight clip is hard-cut to silence.

Reader-mode traversal applies a single rule on every arrival (forward, back, branch click, or auto-advance):

```
On arrival at waypoint W:
    if W.audioId != none:
        hard-cut current clip → play W.audioId from start (loop iff W.audioLoops)
    elif W.stopAudio:
        hard-cut current clip → silence
    else:
        recursively resolve from W's first incoming edge source
        (or silence if W has no incoming edge)
```

That single rule covers forward navigation, Back through history, branch picks, and transition-point auto-advance. The chain's edges *are* the source of truth for "what should be playing where" — same way the chain's edges define the reading order. The reader never has to think about audio; the artist never has to think about a timeline.

Author-mode is silent: clip playback is gated entirely on `ReaderMode::is_active()`. Audio is editor-output, not editor-input.

## 2. Inheritance from existing systems

Everything load-bearing already exists:

- **`Waypoint` data model.** The skin field (PHASE1 §5a) is already a NetObj-synced binary payload referenced from a waypoint; audio adds three sibling fields on the same pattern.
- **`ResourceManager` + `ResourceData`.** The existing image-embedding path (`add_resource_file` / `add_resource`, see `add_file_to_canvas_by_data` in `DrawingProgram.cpp:778`) already stores arbitrary binary blobs (`shared_ptr<string>`) by `NetObjID`, with full NetObj sync and `.inkternity` serialisation. mp3 bytes ride this path verbatim — no new resource type.
- **`ReaderMode::navigate_to`** (and `auto_advance_to`, and `back`, and `forward`) all funnel through a single set-current path. One audio hook attached there covers every traversal kind.
- **`Waypoint::isTransition` + `stopTime`** (TRANSITIONS.md §5). Transition points fire the same `navigate_to` flow during auto-advance, so audio rules apply to transition arrivals identically. A transition-point chain `A (audio) → P1 → P2 → B` plays A's audio across the auto-played pan, exactly as the artist would expect.
- **`WaypointTool` settings panel.** Already renders per-waypoint controls (label, transition speed, easing, transition checkbox, frame-step buttons). Adds an Audio block in the same shape.

No new resource pipeline, no new sync surface, no parallel data-model branch.

## 3. Data model

Add three fields to `Waypoint`:

```cpp
NetworkingObjects::NetObjID audioId{};   // {0,0} == no audio attached
bool                        audioLoops = false;
bool                        stopAudio  = false;
```

**Cumulative audio budget per canvas: hard cap at 30 MB.** Sum the on-disk size of every distinct `ResourceData` referenced by some waypoint's `audioId`. Dedup is on resource id — N waypoints pointing at the same mp3 cost 1× its size, not N×. The cap is enforced at attach time in `WaypointTool` (see §4), not at save time, so the artist sees the failure on the action that caused it rather than on a deferred save. The constant lives next to the other Waypoint constants:

```cpp
static constexpr size_t TOTAL_AUDIO_BUDGET_BYTES = 30u * 1024u * 1024u;  // 30 MB
```

Rationale for choosing 30 MB and not something larger: a 30-second mp3 at 128 kbps is ~480 KB, so 30 MB buys ~30 min of distinct audio (or ~60 min at 64 kbps, which is plenty for narration / ambient cues). The `.inkternity` autosave path rewrites the full file every 3 minutes (commit `806cf78`) — a 30 MB ceiling keeps that write under the I/O-spike threshold even on lower-end drives. Files that need more cumulative audio are signal that the work has outgrown the embedded-bytes model; that's where the deferred Stream-only mode in §14 picks up.

`audioId == {0,0}` is the default for every existing waypoint and produces exactly pre-existing behavior. `stopAudio == false` is a no-op default. `audioLoops` is meaningful only when `audioId` is non-null.

Getter/setter shape mirrors the existing transition fields:

```cpp
NetworkingObjects::NetObjID get_audio_id() const { return audioId; }
void set_audio_id(NetworkingObjects::NetObjID id) { audioId = id; }
bool has_audio()   const { return audioId.is_valid(); }

bool get_audio_loops() const { return audioLoops; }
void set_audio_loops(bool v) { audioLoops = v; }
bool& mutable_audio_loops()  { return audioLoops; }

bool get_stop_audio() const { return stopAudio; }
void set_stop_audio(bool v) { stopAudio = v; }
bool& mutable_stop_audio()  { return stopAudio; }

static void publish_audio_id_update(const NetObjTemporaryPtr<Waypoint>& o);
static void publish_audio_loops_update(const NetObjTemporaryPtr<Waypoint>& o);
static void publish_stop_audio_update(const NetObjTemporaryPtr<Waypoint>& o);
```

**Why three flags, not a single enum:** `audioId` and `stopAudio` are orthogonal in principle. A waypoint with both an audio clip *and* `stopAudio` set is contradictory but easy to handle (audio wins; stop is silently ignored — or vice versa; document the precedence). Modelling them as separate fields keeps each setter symmetric with the existing per-field Net broadcast pattern. The combination "has audio AND stopAudio" is collapsed at evaluation time, not at storage time, so the artist can toggle one without losing the other.

Precedence: **`audioId` wins over `stopAudio` when both are set on the same waypoint.** Rationale: the artist explicitly attached audio, that's a stronger signal than a leftover stop flag.

## 4. Authoring UI

The `WaypointTool` settings panel today shows label / transition speed / easing / transition-point flag / stop-time slider, plus the FRAME_ANIM frame-step + copy-frame controls. Append an **Audio** block below:

```
+----------------------------------+
| (existing waypoint settings)     |
+----------------------------------+
| Audio                            |
| [ Attach audio file... ]         |  <- when no clip attached
| -- or, when clip is attached --  |
| File: <name.mp3>     [ Clear ]   |  <- shows resource name
| ☐ Loop                           |
+----------------------------------+
| ☐ Stop audio at this waypoint    |  <- independent, always shown
+----------------------------------+
```

- **Attach audio file...** opens the OS file picker (reuse the existing `MainProgram::input_add_file_to_canvas_callback` path, but route to `add_resource_file` and assign the returned `NetObjID` to the waypoint instead of dropping it as an `ImageCanvasComponent`). On macOS / Windows the picker filters to `*.mp3` (defensive: the resource pipeline will accept whatever bytes you give it, but we want to fail at picker time on obviously wrong files).
- **Budget check before attach.** Before calling `add_resource_file`, the panel queries `ResourceManager` for the running total of audio-resource bytes already referenced by waypoint `audioId`s. If `(currentTotal + newFileSize) > Waypoint::TOTAL_AUDIO_BUDGET_BYTES` (30 MB), the attach is refused and a one-line error renders inline on the panel: "Audio budget exceeded — canvas total is N.N MB / 30 MB, file would add M.M MB." Refusing in the panel keeps the resource pool clean (no orphaned `ResourceData` added then never referenced). Replacing an existing clip (Clear + Attach) is handled naturally: Clear drops the old reference, the running total drops if no other waypoint references the same id, then the Attach budget check has the freed headroom.
- **Cumulative-budget readout.** A muted-style label below the Attach button shows the running total at all times when a clip is attached: "Canvas audio: N.N MB / 30 MB." Helps the artist plan without having to attach-and-fail.
- **Clear** drops `audioId` back to `{0,0}`. Doesn't delete the `ResourceData` (other waypoints may reference it; ResourceManager handles refcount-style cleanup on save).
- **Loop** is hidden when no clip is attached.
- **Stop audio at this waypoint** is independent. Shown even when no clip is attached (the artist may want to mark a waypoint as "silence anchor").

No volume slider in v1. No fade controls. No trimming. If the artist wants a 3-second clip out of a 30-second mp3, they edit the mp3 externally.

### Visual indication on the marker

Mark waypoints that carry audio with a small note-glyph badge in `WaypointCanvasComponent::draw`. Same pattern as the existing "skin tints the marker accent-pink" indicator. Stop-audio waypoints get a different glyph (a small `||` or a strikethrough on the note glyph). Both indicators are tool chrome — they vanish in reader mode the same way the rest of the marker does.

## 5. Reader-mode behavior

### The arrival rule

The hook attaches to `ReaderMode::set_current` (which every navigate-path funnels through). The rule:

```cpp
void ReaderMode::apply_audio_rule_for(NetObjID waypointId) {
    NetObjID resolved = resolve_audio_for(waypointId);
    if (resolved == lastPlayedAudioId)
        // Re-fire even if same id — Back / branch revisits should
        // restart the clip from the beginning per the design's
        // "audio replays from the predecessor" rule.
        ;
    AudioEngine::stop_current();
    if (resolved.is_valid())
        AudioEngine::play(resolved, loops_for(resolved), /*fromStart=*/true);
    lastPlayedAudioId = resolved;
}

NetObjID ReaderMode::resolve_audio_for(NetObjID waypointId,
                                       int recursionGuard = MAX_CHAIN_HOPS) {
    if (recursionGuard <= 0) return {0,0};
    auto wpRef = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(waypointId);
    if (!wpRef) return {0,0};
    if (wpRef->has_audio())  return wpRef->get_audio_id();
    if (wpRef->get_stop_audio()) return {0,0};  // explicit silence
    // Walk back via first incoming edge.
    NetObjID predecessor = first_incoming_source_of(waypointId);
    if (!predecessor.is_valid()) return {0,0};  // chain origin with no audio
    return resolve_audio_for(predecessor, recursionGuard - 1);
}
```

`MAX_CHAIN_HOPS` caps at `MAX_TRANSITION_CHAIN` (32, already defined in `ReaderMode.hpp` for the transition cycle guard). The same artist-mistake-budget applies — if the audio resolution walk goes deeper than that, treat as silence rather than spinning.

### How each navigation kind triggers the rule

- **Forward** (`forward()`) → calls `navigate_to(targetId)` → which calls `set_current` → which fires the audio rule.
- **Back** (`back()`) → pops history → calls `set_current(poppedId)` → fires the audio rule. This is where the "replays from the predecessor" behavior comes from: `resolve_audio_for(poppedId)` either returns the back-target's own audio or walks up the chain until it finds an audio-bearing ancestor.
- **Branch click** → `navigate_to(branchTargetId)` → fires the rule.
- **Auto-advance** (transition chain) → `auto_advance_to(nextId)` → calls `set_current(nextId)` → fires the rule. A transition point with audio plays its audio across the auto-played pan. A transition point with `stopAudio` silences the chain mid-flight. Both are useful (the artist can put a cue at the start of a multi-step camera move, or use a transition to mark the end of a track without a hard navigation stop).
- **`set_active(false)`** (toggle reader mode off) → unconditionally `AudioEngine::stop_current()`. Author mode is silent.

### Why "first incoming wins"

Multiple incoming edges to a single waypoint is rare (only in branched chains where two paths converge). For the audio walk, "first incoming wins" matches the **same tiebreaker used elsewhere** (TRANSITIONS.md §5, FRAME_ANIM.md Copy Frame). If artists actually build convergent chains where the two predecessors carry different audio, they get the first-edge clip; if that's wrong they can solve it by attaching audio explicitly to the convergent waypoint.

### Why every arrival restarts the clip

Per the design decision: "audio replays from the predecessor." Concretely, `set_current(X)` always calls `AudioEngine::stop_current()` then `play(resolved, loops, fromStart=true)`. Even if `resolved == lastPlayedAudioId`, we restart. The simpler alternative — "if same clip, keep playing" — is appealing but breaks the "every waypoint plays its audio the same way" promise. A reader pressing Back from a silence beat to a music beat hears the music start, not pick up where it left off.

This is one branch on a single boolean in one place. If it turns out artists actually want continuity across Back-revisits, swap to "play only if `resolved != lastPlayedAudioId`" — one-line change in `apply_audio_rule_for`.

## 6. The audio engine

A new `AudioEngine` (lives at `src/Audio/AudioEngine.{cpp,hpp}`, instantiated on `MainProgram`, like `ResourceManager`):

```cpp
class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();

        // Async-safe: stops the in-flight clip cleanly (hard cut),
        // releases its decoded PCM, idles the audio device.
        void stop_current();

        // Resolves `id` against ResourceManager, decodes the mp3 to
        // PCM if not already cached, hands the PCM to the audio
        // backend, sets looping. Hard-cuts any in-flight clip first
        // (so callers don't need to call stop_current themselves).
        void play(NetObjID resourceId, bool loops, World& world);

    private:
        NetObjID    currentId{};
        bool        currentLoops = false;
        // ... backend-specific handle (see §7)
};
```

`MainProgram` calls `audioEngine.stop_current()` on shutdown. Reader-mode toggle-off also calls it. Switching files / opening a new canvas also calls it (audio is per-`World`, not global session).

The engine owns one in-flight clip at a time. No mixing. No queue. New clip starts → old one stops first.

## 7. Audio library choice

Currently no audio decoder is in the Conan deps; SDL3 is built with `pulseaudio: False` (the SDL audio subsystem is still compiled in, just without that backend on Linux). We need an mp3 decoder.

Two viable paths:

| Option | What | Pros | Cons |
|---|---|---|---|
| **miniaudio** (single-header) | Handles decode + playback in one library; manages its own audio device | One file in `deps/miniaudio/`, no Conan changes; uniform API for play / loop / stop; mp3 + ogg + wav + flac | Opens its own audio device — needs to coexist with SDL3's audio subsystem (in practice both can request the same OS device; the OS arbitrates) |
| **dr_mp3 + SDL3 audio stream** | dr_mp3 decodes mp3 → PCM samples; SDL3's `SDL_OpenAudioDeviceStream` plays the PCM | Reuses the SDL3 stack we already build against; no second audio backend | Two pieces to wire; the SDL3 audio stream callback model has more surface area to get right |

**Ship miniaudio.** The unified API is meaningfully simpler for v1, and miniaudio has been used in production audio apps for years — its default backends are conservative (WASAPI on Windows, CoreAudio on macOS, ALSA/PulseAudio on Linux, OpenSL on Android) and don't fight SDL. License is public-domain / MIT-0 — no concerns. If a device-arbitration issue surfaces on a specific platform later, the fallback to dr_mp3 + SDL3 stream is well-trodden territory.

Conan/CMake change: drop the `miniaudio.h` single header into `deps/miniaudio/` and `target_include_directories(main PRIVATE deps/miniaudio)`. No new Conan recipe needed.

Two cite the actual miniaudio calls for documentation completeness:
- `ma_engine_init(NULL, &engine)` — once, on AudioEngine ctor.
- `ma_sound_init_from_memory(&engine, mp3Bytes, mp3Len, ...)` — per clip, decoded lazily.
- `ma_sound_set_looping(&sound, loops)`.
- `ma_sound_start(&sound)` / `ma_sound_stop(&sound)`.
- `ma_engine_uninit(&engine)` — on AudioEngine dtor.

PCM is decoded on demand inside miniaudio; we don't keep our own decoded buffer.

## 8. File format

Bump `INFPNT000013` → `INFPNT000014` (current header is `0.11.0`; bump to `0.12.0`). If FRAME_ANIM landed a header bump before AUDIO, take the next one; check at implementation time.

`Waypoint::save_file` appends, after the existing FRAME_ANIM-or-transition block:

```cpp
a(audioId);
a(audioLoops);
a(stopAudio);
```

Add `Waypoint::load_audio_data_from_archive(a, version)` gated on file version `>= 0.12.0`. Older files default to `audioId = {0,0}, audioLoops = false, stopAudio = false` — identical pre-existing behavior.

The audio bytes themselves live in `ResourceManager`'s resource list (same as image bytes), so they're already serialised by the existing resource path — `Waypoint` only stores the `NetObjID` reference.

Wire the new load helper into `WaypointGraph::load_file` alongside the existing gated reads.

## 9. NetObj sync

Extend `WaypointCommand` enum with three new tags:

```cpp
SET_AUDIO_ID     = 8,
SET_AUDIO_LOOPS  = 9,
SET_STOP_AUDIO   = 10,
```

…each with the publish / server-handle / client-handle triple. Scalar payloads; trivial sync. `SET_AUDIO_ID` carries a `NetObjID` (8 bytes); the audio bytes themselves sync via the existing `ResourceData` registration path (mp3 is registered in `ResourceManager` first, which broadcasts the resource creation; the waypoint then references it by id).

Extend `write_constructor_data` and `readConstructorData` to round-trip the three new fields at the end of the existing block. Newly-joined subscribers receive the fields via the initial-state snapshot, in addition to live updates.

## 10. Edge cases

| Case | Behavior |
|---|---|
| Waypoint has audio AND stopAudio set | Audio wins (the artist explicitly attached a clip). stopAudio is silently ignored. Document; don't surface a warning. |
| Audio clip's resource hasn't loaded yet (collab subscriber mid-sync) | `AudioEngine::play` no-ops if the resource isn't resolvable. The arrival rule still runs (current clip stops if any); the new clip simply doesn't start. Picks up on next navigation. |
| Reader presses Back many times rapidly | Each Back fires `set_current` → audio rule re-fires → previous clip stops + new one starts. Mid-decode is hard-cut. miniaudio handles this safely; the decoder discards in-flight samples on `ma_sound_stop`. |
| Toggle reader-mode off during playback | `set_active(false)` → `stop_current()`. Silence. Toggle back on → camera snaps to current waypoint → audio rule fires from the current waypoint. (This is an entry-point arrival, same as the first toggle-on.) |
| Switch to another canvas / open a different file mid-playback | Per-`World` `AudioEngine` reference is dropped; its dtor stops the clip. New world has its own silent state until the artist re-enters reader mode. |
| Transition with audio AND short stopTime | Transition's audio plays starting at arrival, auto-advance kicks the next move after `stopTime`. If the next waypoint also has audio, hard-cut to that one — even if the transition's clip is mid-play. Same hard-cut as any other transition. |
| Cycle: A (audio X) → P → A | Audio resolution doesn't loop unless `audioLoops` is set on A's clip. The chain cycle is caught by the existing `MAX_TRANSITION_CHAIN` guard in reader mode. Audio rule fires once per arrival, so a chain hop firing A → A → A doesn't queue multiple plays — each new arrival hard-cuts and restarts. |
| Multiple waypoints reference the same mp3 (`audioId`) | ResourceData is shared; only one copy is stored in the .inkternity file. miniaudio decodes lazily per play — but since the bytes don't change between waypoints, decoded PCM could be cached on the engine if profiling shows decoding cost dominates. Defer until needed. |
| Author-mode while reader-mode is also active (split window?) | Reader-mode is per-canvas, not per-window. Author UI hides while reader mode is on for that canvas. Audio plays per the current `World`'s reader state. Out of scope for v1 to handle multiple simultaneous readers. |
| Eraser hits an audio-bearing waypoint marker | Same protection as waypoints/transitions: ERASER skips `CanvasComponentType::WAYPOINT`. When the waypoint is deleted (via the tree-view or its `WaypointCanvasComponent` being erased), `WaypointGraph::erase_waypoint_by_id` already sweeps the node + edges + layout entry; nothing audio-specific to add — `audioId` rides with the waypoint. The `ResourceData` is not deleted (other waypoints may reference it; resource cleanup happens at save-time via `get_used_resources`). |
| Attached file would push cumulative audio over 30 MB | Refused at attach time with an inline message ("Audio budget exceeded — canvas total is N.N MB / 30 MB, file would add M.M MB"). No `ResourceData` is added; the artist either picks a smaller / shorter / lower-bitrate file, or clears another waypoint's audio first. |
| Artist attaches a single file > 30 MB | Same path — refused at attach. The error message reads naturally because `(0 + 35MB) > 30MB` produces the same readout shape. |
| Artist plays a non-mp3 file (renames .ogg → .mp3) | miniaudio's auto-detect handles ogg / wav / flac natively, so it'll just work. If it can't decode (corrupt file, encrypted, exotic codec), `ma_sound_init_from_memory` returns an error and `AudioEngine::play` no-ops. Logged via `Logger::get().log("WORLDFATAL", ...)`; no UI cleanup needed because the waypoint still has the bytes attached — the artist can clear and reattach. |
| User has no audio output device (headless / muted) | miniaudio falls back to a null device that silently consumes samples. No crash; reader sees no audio cues but everything else works. |

## 11. Subscription / collab playback (the hard part)

Bytes-on-the-wire for the mp3 itself is solved by the existing `ResourceData` path — same machinery that already moves embedded images and brush-preset icons across the network. The bytes flow once per resource at canvas-snapshot time; downstream waypoints reference by `NetObjID`. The bandwidth cost is concentrated at initial connect: a subscriber joining a canvas with 40 MB of attached audio downloads 40 MB before the canvas is fully usable. That's the same shape of cost the existing image-embedding path has — bigger payloads, same plumbing.

**The real hard part is playback synchronisation.** `ReaderMode` is currently per-client local state (`ReaderMode.hpp:113`'s `currentId` is not NetObj-synced; each client toggles their own reader mode independently). When the host walks A → B → C in reader mode, the host's local `set_current` fires the audio rule on the host's speakers. Subscribers see the host's camera (via existing `DrawCamera` sync) but the audio rule doesn't fire on their side — their `ReaderMode::is_active()` returns false.

Three options, ordered by complexity:

| Option | What | When |
|---|---|---|
| **A. No subscriber audio (v1)** | Bytes sync into the file via existing resources. Playback is local-machine-only: whoever has reader mode active on their own machine hears the cues. Subscribers see the camera moves silently. A subscriber who later downloads the `.inkternity` and opens it standalone gets full audio. | **Ship this in v1.** Zero new sync surface. |
| **B. Mirror host's reader-mode state** | NetObj-sync the host's `currentId` + an "audio rule fired" event. Subscribers fire their own local audio rule when they receive the event; the resource is already on their side from the initial-state snapshot. Latency = network round-trip (typically <100 ms). | v2 — requires syncing `ReaderMode` itself, which is a separate small piece of work. |
| **C. Stream mixed audio as a media track** | Open a WebRTC audio channel, mix on host, stream to subscribers. | Out of scope; doesn't fit the NetObj architecture. |

The doc plans for **(A)**. The audio engine's `play` / `stop` calls live entirely on the local `MainProgram` and never broadcast events. The mp3 bytes ride existing resource sync so the *file* stays portable. If artists ask for sub-side audio later, **(B)** is the documented escape hatch — and it's a contained piece of work (one or two new `ReaderMode`-command NetObj messages, fire `apply_audio_rule_for` on the subscriber side).

There's an implicit corollary worth being explicit about: for a subscriber-friendly performance, the artist's workflow is either (a) record the session externally with audio (OBS / screen-recording captures both the camera moves and the host-side audio), or (b) hand the `.inkternity` file to the audience to open locally. Live-presented-with-audio is a v2 promise, not a v1 one.

**Bandwidth-control mitigations** (still applicable in v1 since the bytes are on the wire regardless):

- The **hard 30 MB cumulative cap** (`Waypoint::TOTAL_AUDIO_BUDGET_BYTES`, §3) bounds the worst case at attach time. Initial-connect bandwidth for a maxed-out canvas tops out at 30 MB; the host's subsequent autosaves write 30 MB per cycle at most.
- The existing `ResourceManager` dedup means N waypoints referencing the same mp3 cost N×8 bytes (the `NetObjID`s) on top of one mp3 payload — *not* N copies. Counted once against the 30 MB budget.
- The existing autosave path (`806cf78`) rewrites the full file every 3 minutes for canvases with unsaved changes. With the 30 MB cap, even a maxed-out canvas writes 30 MB per cycle — within range of the SEH-guarded atomic-rename save on a typical drive. Worth surfacing in MANUAL.md under "audio-heavy canvases save more slowly."

## 12. Risks

- **File-size bloat.** Bounded by the 30 MB cumulative cap (§3). A maxed-out canvas writes 30 MB per autosave cycle — within the comfortable range of the SEH-guarded atomic-rename save on typical drives. Artists who genuinely need more cumulative audio than 30 MB are the signal that the embedded-bytes model no longer fits their use case; the deferred Stream-only mode (§14) is the path forward for them.
- **mp3 patent landscape.** mp3 patents expired in 2017 (US) / 2012 (EU). miniaudio's mp3 decoder is patent-free. No legal concern.
- **Latency on first play.** miniaudio decodes on-demand starting at `ma_sound_init_from_memory`. For mp3 this is fast (single ms for typical clips); the audio rule fires synchronously inside `set_current` and the decode completes before the camera smooth-move finishes. If profiling shows otherwise, switch to a background-thread decode and accept ~50ms of silence at the start of each clip.
- **miniaudio vs. SDL3 audio device contention.** Both want the OS audio device. In practice WASAPI / CoreAudio / ALSA allow multiple concurrent clients (the OS mixes them). PulseAudio on Linux ditto. If a specific platform turns out to fail, the dr_mp3 + SDL3 audio stream fallback is well-trodden — the engine is small enough (~150 lines) that swapping the backend is contained.
- **Cycle in the audio-resolution walk.** Possible if the graph itself has a cycle (e.g. A → B → A). The `MAX_CHAIN_HOPS` cap (32, shared with transitions) bails before the walk loops. Logged once per cycle so the artist can see they have a structural problem.
- **Hard cut click artifacts.** Hard-cutting in the middle of an mp3 sample can audibly click. miniaudio uses a fade-out internally on `ma_sound_stop` (a few ms ramp to silence), so this is usually inaudible. If artists report clicks, the crossfade option from the design discussion is the path forward (10–50ms fade).
- **Auto-advance + audio is psychologically tricky.** Setting a clip on a transition that auto-advances at `stopTime = 0.1s` would play 100ms of audio then hard-cut to silence (or to the next clip). That's correct per the rule but probably surprising. Behavior is what the artist authored; no special-case needed.

## 13. Milestones

| | Deliverable |
|---|---|
| A1 | Vendor `deps/miniaudio/miniaudio.h` (latest stable); wire `target_include_directories(main PRIVATE deps/miniaudio)` and a single `miniaudio.c` translation unit that defines `MINIAUDIO_IMPLEMENTATION`. |
| A2 | `AudioEngine` class: `ma_engine` init/uninit in ctor/dtor, `play(id, loops, world)`, `stop_current()`. Lives on `MainProgram`. Wired to call `stop_current()` on world-switch and exit. |
| A3 | `Waypoint` data model: `audioId` + `audioLoops` + `stopAudio` fields, getters/setters/mutables, NetObj constructor payload extension, save/load gated on `>= 0.12.0`, format header bump. |
| A4 | `WaypointCommand` extension: 3 new tags + publish/server-handle/client-handle triples. Symmetric `write_constructor_data` extension. |
| A5 | `WaypointTool` settings panel: Audio block (Attach button + clear + loop + stop). Marker glyph for has-audio / stop-audio in `WaypointCanvasComponent::draw`. |
| A6 | `ReaderMode::apply_audio_rule_for` + `resolve_audio_for` (recursive incoming-edge walk capped at MAX_CHAIN_HOPS). Hook into `set_current` so every navigate-path fires it. Wire `set_active(false)` → `stop_current`. |
| A7 | Manual test pass: 3-waypoint chain A (clip, loop=true) → B → C (stopAudio); verify A plays from arrival, continues through B, stops at C, restarts on Back to A, restarts on Back to B (resolves via A); reader-mode toggle off mid-play silences cleanly; attaching a file that would push the canvas past 30 MB refuses cleanly with the inline message and leaves `ResourceManager` clean (no orphan); the budget readout updates after Clear. Sub-side playback is **explicitly deferred** (§11 option A — the mp3 bytes still sync via existing resources, but subscribers don't fire the audio rule). |
| A8 | Docs: MANUAL.md "Audio cues" section under reader mode; README bullet under "What Inkternity adds." |

A1–A4 are pure additive plumbing (data + sync + decoder vendor). A5 is the UI surface. **A6 is the load-bearing piece** — the resolve-audio walk has to be right under back-history + branch + transition chains. A7's manual test pass is the safety net.

## 14. Out of scope

- **Stream-only audio mode** — the audio-as-IP / self-sovereign-subscriber model where the bytes stay on the host's disk and are streamed live to subscribers via an Opus track on the existing libdatachannel peer connection. Strong product argument (music-artist commissions an animated album, subs pay for the live performance not for the file, content protection is same-as-Spotify-grade rather than unzip-the-archive-grade). **Explicitly deferred from v1.** Folding it in would need: per-waypoint storage-mode field (Embed vs Stream-only), host-side audio capture + Opus encode, sub-side jitter buffer + Opus decode, mid-clip cold-start handling, ephemeral-key signing on the RTP track via the portal identity layer. Multi-week feature; doesn't share much code with v1's embedded path beyond the playback-rule §5 — most of the work is in the wire layer. v1's 30 MB cap (§3) is the soft signal that an artist is running into the embedded model's limits and may want this. AUDIO_STREAM.md will pick it up when we get there.
- **Multiple simultaneous tracks / mixing.** One in-flight clip at a time. If the artist wants ambient + dialogue, they pre-mix to a single mp3.
- **Volume / per-waypoint mix.** No volume slider. Use a quieter source mp3.
- **Crossfade between clips.** Hard cut only. Revisit if hard-cut clicks turn out to be a real problem.
- **Fade in / fade out controls on a clip.** Bake fades into the mp3 itself.
- **Seek / scrub / pause.** Reader has no audio controls; audio is a side-effect of navigation, not a player.
- **Looping ranges (loop a 4-second slice of a 60-second mp3).** Whole-clip loop only.
- **Per-edge audio override.** Audio belongs to nodes, not edges, in v1.
- **Spatial / positional audio.** No 3D / panning.
- **External-file references** (path-on-disk rather than embedded bytes). Defer until file-size bloat actually hurts — and at that point, the Stream-only mode above is probably the right answer rather than path references.
- **Audio shown in author mode** (audition while editing). The artist can toggle reader mode on briefly to audition; a dedicated "audition this waypoint's audio" button is a polish item.
- **Audio in scene exports / GIF / video.** The screenshot pipeline doesn't carry audio. A future video-export feature would be where audio rendering fits.
- **Soft mute toggle in author mode** (some artists hate when audio plays at all). Author-mode is unconditionally silent in v1 — there is no situation where audio plays without `ReaderMode::is_active()` being true.
