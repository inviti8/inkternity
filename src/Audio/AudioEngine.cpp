#include "AudioEngine.hpp"

#include "../World.hpp"
#include "../ResourceManager.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp>

AudioEngine::AudioEngine() {
    ma_result result = ma_engine_init(nullptr, &engine_);
    if (result != MA_SUCCESS) {
        // No audio device on this machine, or the OS denied access.
        // Not fatal — play() / stop_current() become no-ops; the rest
        // of the app keeps working in silence.
        Logger::get().log("INFO",
            std::string("[AudioEngine] ma_engine_init failed (") +
            std::to_string(static_cast<int>(result)) +
            "); audio playback disabled.");
        engineReady_ = false;
        return;
    }
    engineReady_ = true;
}

AudioEngine::~AudioEngine() {
    stop_current();
    if (engineReady_) {
        ma_engine_uninit(&engine_);
        engineReady_ = false;
    }
}

void AudioEngine::stop_current() {
    // Teardown order matters: the ma_sound holds a pointer into the
    // ma_decoder (via ma_sound_init_from_data_source), and the
    // decoder holds a pointer into the bytes buffer. Tear down in
    // reverse: sound -> decoder -> bytes.
    if (currentSound_) {
        // ma_sound_uninit is safe to call mid-play; miniaudio applies
        // a short internal fade-out to suppress click artefacts on the
        // hard cut.
        ma_sound_uninit(currentSound_.get());
        currentSound_.reset();
    }
    if (currentDecoder_) {
        ma_decoder_uninit(currentDecoder_.get());
        currentDecoder_.reset();
    }
    currentBytes_.reset();
    currentResourceId_ = NetworkingObjects::NetObjID{};
}

void AudioEngine::play(NetworkingObjects::NetObjID resourceId, bool loops, World& world) {
    if (!engineReady_) return;
    if (resourceId == NetworkingObjects::NetObjID{}) {
        // Nothing to play — treat as silence. Caller may pass an
        // empty id when the audio rule resolved to "no audio".
        stop_current();
        return;
    }

    auto resourceRef = world.netObjMan.get_obj_temporary_ref_from_id<ResourceData>(resourceId);
    if (!resourceRef) {
        // Resource isn't sync'd yet (subscriber mid-connect, or stale
        // id pointing at a deleted resource). Stop whatever is in
        // flight; the audio rule will re-fire on the next arrival.
        Logger::get().log("INFO",
            "[AudioEngine] play() called with unresolvable resource id; staying silent.");
        stop_current();
        return;
    }

    // Capture the bytes BEFORE we tear down whatever's currently
    // playing, so a self-overlay (same id playing again) doesn't free
    // the buffer between stop and start.
    std::shared_ptr<std::string> newBytes = resourceRef->data;
    if (!newBytes || newBytes->empty()) {
        Logger::get().log("INFO",
            "[AudioEngine] resource has no bytes; staying silent.");
        stop_current();
        return;
    }

    // Hard cut anything in flight. miniaudio's internal fade-out
    // makes this audibly clean.
    stop_current();

    // Two-stage pipeline. The decoder reads from `newBytes` (its
    // memory buffer) and exposes a ma_data_source interface; the
    // sound plays that data source. Decoder format is auto-detected
    // (mp3 / ogg / wav / flac); a corrupt blob fails the decoder
    // init, never the sound init.
    auto newDecoder = std::make_unique<ma_decoder>();
    ma_result result = ma_decoder_init_memory(
        newBytes->data(),
        newBytes->size(),
        nullptr,
        newDecoder.get());
    if (result != MA_SUCCESS) {
        Logger::get().log("WORLDFATAL",
            std::string("[AudioEngine] ma_decoder_init_memory failed (") +
            std::to_string(static_cast<int>(result)) +
            "); audio not started.");
        return;
    }

    auto newSound = std::make_unique<ma_sound>();
    result = ma_sound_init_from_data_source(
        &engine_,
        newDecoder.get(),
        0,        // flags: no async load, no spatial, no streaming-from-disk
        nullptr,  // sound group: default (master)
        newSound.get());
    if (result != MA_SUCCESS) {
        Logger::get().log("WORLDFATAL",
            std::string("[AudioEngine] ma_sound_init_from_data_source failed (") +
            std::to_string(static_cast<int>(result)) +
            "); cleaning up.");
        ma_decoder_uninit(newDecoder.get());
        return;
    }

    ma_sound_set_looping(newSound.get(), loops ? MA_TRUE : MA_FALSE);

    result = ma_sound_start(newSound.get());
    if (result != MA_SUCCESS) {
        Logger::get().log("WORLDFATAL",
            std::string("[AudioEngine] ma_sound_start failed (") +
            std::to_string(static_cast<int>(result)) +
            "); cleaning up.");
        ma_sound_uninit(newSound.get());
        ma_decoder_uninit(newDecoder.get());
        return;
    }

    currentSound_      = std::move(newSound);
    currentDecoder_    = std::move(newDecoder);
    currentBytes_      = std::move(newBytes);
    currentResourceId_ = resourceId;
}
