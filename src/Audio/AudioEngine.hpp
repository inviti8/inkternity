#pragma once
#include "miniaudio.h"
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <memory>
#include <string>

class World;

// AUDIO.md §6 — owns the platform audio device and at most one
// in-flight mp3 stream. Lives on MainProgram (not World) so a single
// engine survives world switches; clip lifetime is per-`play()` call,
// and switching worlds explicitly stop_current()s.
//
// The engine is intentionally one-clip-at-a-time. No mixing, no
// queue: a new `play()` hard-cuts whatever's in flight (per the
// design's "hard cut" decision). Loop / no-loop is a per-call flag.
//
// Thread model: every public method is meant to be called from the
// main thread. miniaudio's own audio callback runs on its own
// device thread internally; ma_engine + ma_sound are documented as
// safe to call across threads, but our callers are all main-thread
// (ReaderMode::set_current, MainProgram::early_destroy, etc.), so no
// extra locking is needed on our side.
class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();

        // Non-copyable / non-movable — owns a heap-allocated ma_engine
        // and ma_sound and holds raw pointers into the resource bytes
        // it's currently streaming from.
        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;
        AudioEngine(AudioEngine&&) = delete;
        AudioEngine& operator=(AudioEngine&&) = delete;

        // Hard-cuts the in-flight clip (uninit + release bytes). Safe to
        // call when nothing is playing.
        void stop_current();

        // Resolves `resourceId` against the world's ResourceManager,
        // hard-cuts any in-flight clip, decodes + starts the new one.
        // No-op (with a Logger warn) if:
        //   - the engine never initialised (no audio device available),
        //   - the resource id is invalid / empty / not yet sync'd in,
        //   - miniaudio can't decode the bytes (corrupt / unsupported).
        // Always starts the clip from the beginning (per the design's
        // "audio replays from the predecessor" rule).
        void play(NetworkingObjects::NetObjID resourceId, bool loops, World& world);

        // True when ma_engine_init succeeded. False on systems with no
        // audio output. Used by the rest of the app for diagnostics only;
        // play() / stop_current() are no-ops when not ready, callers
        // don't need to check this.
        bool ready() const { return engineReady_; }

    private:
        ma_engine engine_{};
        bool      engineReady_ = false;

        // miniaudio's memory-source path is a two-stage pipeline:
        //   bytes -> ma_decoder (mp3/ogg/wav/flac demuxer + PCM out)
        //         -> ma_sound (plays the decoder as a data source)
        // Both objects are heap-allocated so AudioEngine's own
        // footprint stays stable. ma_decoder must outlive ma_sound;
        // the dtor / stop_current() teardown order respects this.
        std::unique_ptr<ma_sound>   currentSound_;
        std::unique_ptr<ma_decoder> currentDecoder_;
        // Shared ownership of the source-of-truth bytes — ma_decoder
        // reads from this memory directly, so the buffer must outlive
        // the decoder. Same shared_ptr the ResourceData holds, so
        // multiple waypoints referencing the same mp3 don't duplicate.
        std::shared_ptr<std::string> currentBytes_;
        // For logging only; lets us identify the in-flight clip.
        NetworkingObjects::NetObjID currentResourceId_{};
};
