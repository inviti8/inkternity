#include "Waypoint.hpp"
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include "../World.hpp"
#include "../CommandList.hpp"
#include "../ResourceManager.hpp"
#include "../ScaleUpCanvas.hpp"
#include "WaypointGraph.hpp"
#include <unordered_set>

#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>
#include <include/core/SkData.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkPngEncoder.h>

using namespace NetworkingObjects;

// PHASE2 M5: CSS-style cubic-bezier easing presets. Control points are
// (x1, y1, x2, y2) — the same convention CSS's cubic-bezier() function
// uses, which BezierEasing accepts directly.
Eigen::Vector4f transition_easing_to_bezier_curve(TransitionEasing e) {
    switch (e) {
        case TransitionEasing::LINEAR:      return {0.0f, 0.0f, 1.0f, 1.0f};
        case TransitionEasing::EASE:        return {0.25f, 0.1f, 0.25f, 1.0f};
        case TransitionEasing::EASE_IN:     return {0.42f, 0.0f, 1.0f, 1.0f};
        case TransitionEasing::EASE_OUT:    return {0.0f, 0.0f, 0.58f, 1.0f};
        case TransitionEasing::EASE_IN_OUT: return {0.42f, 0.0f, 0.58f, 1.0f};
    }
    return {0.25f, 0.1f, 0.25f, 1.0f};  // Fallback to EASE on garbage input.
}

const std::vector<std::string>& transition_easing_display_names() {
    // Order MUST match the enum's numeric values so a dropdown can
    // index this list by static_cast<size_t>(TransitionEasing).
    static const std::vector<std::string> names = {
        "Linear",
        "Ease",
        "Ease in",
        "Ease out",
        "Ease in-out"
    };
    return names;
}

Waypoint::Waypoint() {}

Waypoint::Waypoint(const std::string& initLabel,
                   const CoordSpaceHelper& initCoords,
                   const Vector<int32_t, 2>& initWindowSize)
    : label(initLabel),
      coords(initCoords),
      windowSize(initWindowSize) {}

void Waypoint::scale_up(const WorldScalar& scaleUpAmount) {
    coords.scale_about(WorldVec{0, 0}, scaleUpAmount, true);
}

namespace {
// Encode an SkImage as PNG bytes for serialization. Returns empty
// vector on failure (or no skin) — callers treat that as "no skin".
std::vector<uint8_t> encode_skin_png(const sk_sp<SkImage>& img) {
    if (!img) return {};
    SkPixmap pix;
    sk_sp<SkImage> raster = img->makeRasterImage(nullptr);
    if (!raster || !raster->peekPixels(&pix)) return {};
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pix, {})) return {};
    auto skData = stream.detachAsData();
    if (!skData) return {};
    std::vector<uint8_t> out(skData->size());
    std::memcpy(out.data(), skData->bytes(), skData->size());
    return out;
}

sk_sp<SkImage> decode_skin_png(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    auto data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    if (!data) return nullptr;
    auto codec = SkCodec::MakeFromData(data, {SkPngDecoder::Decoder()});
    if (!codec) return nullptr;
    auto [img, _] = codec->getImage();
    return img;
}
}  // namespace

void Waypoint::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(label, coords, windowSize);
    std::vector<uint8_t> skinBytes = encode_skin_png(skin);
    a(skinBytes);
    // PHASE2 M4 / M5: per-waypoint reader-mode transition controls. Always
    // written from format v0.9 onward; the load path gates its read
    // on file version >= 0.9 so older files don't try to consume bytes
    // that aren't there.
    a(transitionSpeedMultiplier);
    a(static_cast<uint8_t>(transitionEasing));
    // TRANSITIONS.md — transition-point fields. Written from format
    // v0.10 onward; load path gates on >= 0.10.
    a(isTransition);
    a(stopTime);
    // AUDIO.md §8 — per-waypoint audio cue: resource id, loop flag,
    // stopAudio flag. mp3 bytes themselves live in ResourceManager
    // and are serialised by the existing resource path; this only
    // writes the reference. Pure additive; no version gating because
    // we don't carry forward older saves.
    a(audioId);
    a(audioLoops);
    a(stopAudio);
}

void Waypoint::load_skin_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    std::vector<uint8_t> skinBytes;
    a(skinBytes);
    skin = decode_skin_png(skinBytes);
}

void Waypoint::load_transition_data_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    a(transitionSpeedMultiplier);
    transitionSpeedMultiplier = std::clamp(transitionSpeedMultiplier, TRANSITION_SPEED_MIN, TRANSITION_SPEED_MAX);
    uint8_t easingByte;
    a(easingByte);
    // Clamp to valid enum range; unknown values fall back to EASE.
    if (easingByte > static_cast<uint8_t>(TransitionEasing::EASE_IN_OUT))
        easingByte = static_cast<uint8_t>(TransitionEasing::EASE);
    transitionEasing = static_cast<TransitionEasing>(easingByte);
}

void Waypoint::load_transition_point_data_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    a(isTransition);
    a(stopTime);
    stopTime = std::clamp(stopTime, TRANSITION_STOP_TIME_MIN, TRANSITION_STOP_TIME_MAX);
}

// AUDIO.md §8 — reads the three audio fields (id + loop + stopAudio).
// Always called unconditionally; we don't preserve older save
// formats (the working assumption is no migration, per the May 2026
// "start clean" decision).
void Waypoint::load_audio_data_from_archive(cereal::PortableBinaryInputArchive& a, VersionNumber) {
    a(audioId);
    a(audioLoops);
    a(stopAudio);
}

// P0.5-LIVE-SYNC: command set for Waypoint NetObj update messages.
// One enum value per editable field. Wire format: `<command_byte> <field_value>`.
enum class WaypointCommand : uint8_t {
    SET_LABEL              = 0,
    SET_TRANSITION_SPEED   = 1,
    SET_TRANSITION_EASING  = 2,
    SET_IS_TRANSITION      = 3,
    SET_STOP_TIME          = 4,
    SET_SKIN               = 5,
    // AUDIO.md §9 — three new tags for the per-waypoint audio cue.
    // Scalar payloads in every case; mp3 bytes ride the existing
    // ResourceData sync, not these messages.
    SET_AUDIO_ID           = 6,
    SET_AUDIO_LOOPS        = 7,
    SET_STOP_AUDIO         = 8
};

void Waypoint::publish_label_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_LABEL, o->label);
        });
}

void Waypoint::publish_transition_speed_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_TRANSITION_SPEED, o->transitionSpeedMultiplier);
        });
}

void Waypoint::publish_transition_easing_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_TRANSITION_EASING, static_cast<uint8_t>(o->transitionEasing));
        });
}

void Waypoint::publish_is_transition_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_IS_TRANSITION, o->isTransition);
        });
}

void Waypoint::publish_stop_time_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_STOP_TIME, o->stopTime);
        });
}

void Waypoint::publish_skin_update(const NetObjTemporaryPtr<Waypoint>& o) {
    // Skin is a PNG byte vector — same encoding the snapshot uses.
    // Larger payload than the scalar fields but fires only on
    // ButtonSelectTool capture, not on continuous interaction.
    std::vector<uint8_t> skinBytes = encode_skin_png(o->skin);
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [&skinBytes](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_SKIN, skinBytes);
        });
}

// AUDIO.md §9 — small scalar broadcasts for the three audio fields.
// Fire on artist actions (attach / clear / toggle), never on
// continuous interaction; no rate-limiting needed.
void Waypoint::publish_audio_id_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_AUDIO_ID, o->audioId);
        });
}

void Waypoint::publish_audio_loops_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_AUDIO_LOOPS, o->audioLoops);
        });
}

void Waypoint::publish_stop_audio_update(const NetObjTemporaryPtr<Waypoint>& o) {
    o.send_update_to_all(RELIABLE_COMMAND_CHANNEL,
        [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
            a(WaypointCommand::SET_STOP_AUDIO, o->stopAudio);
        });
}

size_t Waypoint::compute_canvas_audio_total_bytes(World& w) {
    auto& nodes = w.wpGraph.get_nodes();
    if (!nodes) return 0;
    std::unordered_set<NetObjID> seen;
    size_t total = 0;
    for (auto& info : *nodes) {
        const NetObjID id = info.obj->audioId;
        if (id == NetObjID{}) continue;
        if (!seen.insert(id).second) continue;   // already counted
        auto resourceRef = w.netObjMan.get_obj_temporary_ref_from_id<ResourceData>(id);
        if (resourceRef && resourceRef->data)
            total += resourceRef->data->size();
    }
    return total;
}

void Waypoint::write_constructor_data(const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
    // P0.5-SKINS / P0.5-LIVE-SYNC partial fix: include every
    // post-construction-editable field in the constructor payload so
    // a subscriber's initial-state snapshot reflects the current
    // canvas, not just the construction-time defaults. Symmetric with
    // the file-format save_file order. Live updates for these fields
    // (post-snapshot edits the subscriber should see) is a separate
    // P0.5 task — this only fixes the snapshot path.
    a(o->label, o->coords, o->windowSize);
    a(o->transitionSpeedMultiplier);
    a(static_cast<uint8_t>(o->transitionEasing));
    a(o->isTransition);
    a(o->stopTime);
    std::vector<uint8_t> skinBytes = encode_skin_png(o->skin);
    a(skinBytes);
    // AUDIO.md §9 — extend the snapshot payload so subscribers see
    // existing audio cues on join, not just future edits.
    a(o->audioId);
    a(o->audioLoops);
    a(o->stopAudio);
}

void Waypoint::register_class(World& w) {
    auto readConstructorData = [&w](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryInputArchive& a, const std::shared_ptr<NetServer::ClientData>& c) {
        a(o->label, o->coords, o->windowSize);
        a(o->transitionSpeedMultiplier);
        o->transitionSpeedMultiplier = std::clamp(o->transitionSpeedMultiplier,
                                                  TRANSITION_SPEED_MIN, TRANSITION_SPEED_MAX);
        uint8_t easingByte;
        a(easingByte);
        if (easingByte > static_cast<uint8_t>(TransitionEasing::EASE_IN_OUT))
            easingByte = static_cast<uint8_t>(TransitionEasing::EASE);
        o->transitionEasing = static_cast<TransitionEasing>(easingByte);
        a(o->isTransition);
        a(o->stopTime);
        o->stopTime = std::clamp(o->stopTime, TRANSITION_STOP_TIME_MIN, TRANSITION_STOP_TIME_MAX);
        std::vector<uint8_t> skinBytes;
        a(skinBytes);
        o->skin = decode_skin_png(skinBytes);
        // AUDIO.md §9 — mirror of write_constructor_data: read the
        // three audio fields off the wire so newly-joined subscribers
        // see existing cues on the snapshot.
        a(o->audioId);
        a(o->audioLoops);
        a(o->stopAudio);
        canvas_scale_up_check(*o, w, c);
    };
    // P0.5-LIVE-SYNC: update handlers for every editable Waypoint
    // field. Client side receives broadcast and mutates local state.
    // Server side mutates AND re-broadcasts to all other clients (the
    // canonical fan-out pattern from ClientData::register_class).
    auto readUpdateClient = [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryInputArchive& a, const std::shared_ptr<NetServer::ClientData>&) {
        WaypointCommand cmd;
        a(cmd);
        switch (cmd) {
            case WaypointCommand::SET_LABEL:
                a(o->label);
                break;
            case WaypointCommand::SET_TRANSITION_SPEED:
                a(o->transitionSpeedMultiplier);
                o->transitionSpeedMultiplier = std::clamp(o->transitionSpeedMultiplier,
                                                          TRANSITION_SPEED_MIN, TRANSITION_SPEED_MAX);
                break;
            case WaypointCommand::SET_TRANSITION_EASING: {
                uint8_t b;
                a(b);
                if (b > static_cast<uint8_t>(TransitionEasing::EASE_IN_OUT))
                    b = static_cast<uint8_t>(TransitionEasing::EASE);
                o->transitionEasing = static_cast<TransitionEasing>(b);
                break;
            }
            case WaypointCommand::SET_IS_TRANSITION:
                a(o->isTransition);
                break;
            case WaypointCommand::SET_STOP_TIME:
                a(o->stopTime);
                o->stopTime = std::clamp(o->stopTime, TRANSITION_STOP_TIME_MIN, TRANSITION_STOP_TIME_MAX);
                break;
            case WaypointCommand::SET_SKIN: {
                std::vector<uint8_t> skinBytes;
                a(skinBytes);
                o->skin = decode_skin_png(skinBytes);
                break;
            }
            // AUDIO.md §9 — client-side mutation handlers for the
            // three audio fields. No clamping; the values are either
            // a NetObjID (validated by the resource pipeline at
            // dereference time) or plain booleans.
            case WaypointCommand::SET_AUDIO_ID:
                a(o->audioId);
                break;
            case WaypointCommand::SET_AUDIO_LOOPS:
                a(o->audioLoops);
                break;
            case WaypointCommand::SET_STOP_AUDIO:
                a(o->stopAudio);
                break;
        }
    };
    auto readUpdateServer = [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryInputArchive& a, const std::shared_ptr<NetServer::ClientData>& c) {
        WaypointCommand cmd;
        a(cmd);
        switch (cmd) {
            case WaypointCommand::SET_LABEL:
                a(o->label);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_LABEL, o->label);
                    });
                break;
            case WaypointCommand::SET_TRANSITION_SPEED:
                a(o->transitionSpeedMultiplier);
                o->transitionSpeedMultiplier = std::clamp(o->transitionSpeedMultiplier,
                                                          TRANSITION_SPEED_MIN, TRANSITION_SPEED_MAX);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_TRANSITION_SPEED, o->transitionSpeedMultiplier);
                    });
                break;
            case WaypointCommand::SET_TRANSITION_EASING: {
                uint8_t b;
                a(b);
                if (b > static_cast<uint8_t>(TransitionEasing::EASE_IN_OUT))
                    b = static_cast<uint8_t>(TransitionEasing::EASE);
                o->transitionEasing = static_cast<TransitionEasing>(b);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_TRANSITION_EASING, static_cast<uint8_t>(o->transitionEasing));
                    });
                break;
            }
            case WaypointCommand::SET_IS_TRANSITION:
                a(o->isTransition);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_IS_TRANSITION, o->isTransition);
                    });
                break;
            case WaypointCommand::SET_STOP_TIME:
                a(o->stopTime);
                o->stopTime = std::clamp(o->stopTime, TRANSITION_STOP_TIME_MIN, TRANSITION_STOP_TIME_MAX);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_STOP_TIME, o->stopTime);
                    });
                break;
            case WaypointCommand::SET_SKIN: {
                std::vector<uint8_t> skinBytes;
                a(skinBytes);
                o->skin = decode_skin_png(skinBytes);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [&skinBytes](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_SKIN, skinBytes);
                    });
                break;
            }
            // AUDIO.md §9 — server-side: mutate local state, fan out
            // to every connected client except the originator. Same
            // shape as the existing transition-field handlers.
            case WaypointCommand::SET_AUDIO_ID:
                a(o->audioId);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_AUDIO_ID, o->audioId);
                    });
                break;
            case WaypointCommand::SET_AUDIO_LOOPS:
                a(o->audioLoops);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_AUDIO_LOOPS, o->audioLoops);
                    });
                break;
            case WaypointCommand::SET_STOP_AUDIO:
                a(o->stopAudio);
                o.send_server_update_to_all_clients_except(c, RELIABLE_COMMAND_CHANNEL,
                    [](const NetObjTemporaryPtr<Waypoint>& o, cereal::PortableBinaryOutputArchive& a) {
                        a(WaypointCommand::SET_STOP_AUDIO, o->stopAudio);
                    });
                break;
        }
    };

    w.netObjMan.register_class<Waypoint, Waypoint, Waypoint, Waypoint>({
        .writeConstructorFuncClient = write_constructor_data,
        .readConstructorFuncClient  = readConstructorData,
        .readUpdateFuncClient       = readUpdateClient,
        .writeConstructorFuncServer = write_constructor_data,
        .readConstructorFuncServer  = readConstructorData,
        .readUpdateFuncServer       = readUpdateServer
    });
    register_ordered_list_class<Waypoint>(w.netObjMan);
}
