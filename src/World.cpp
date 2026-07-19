#include "World.hpp"
#include "Diagnostics/RenderStats.hpp"
#include <chrono>
// C2PA/PublishHook.hpp removed — .inkternity sidecar signing not supported
#include <Helpers/CanvasShareId.hpp>
#include <Helpers/HsvRgb.hpp>
#include <Helpers/MathExtras.hpp>
#include <Helpers/Networking/ByteStream.hpp>
#include <Helpers/NetworkingObjects/NetObjManager.hpp>
#include <Helpers/NetworkingObjects/NetObjManagerTypeList.hpp>
#include <Helpers/VersionNumber.hpp>
#include <Helpers/NetworkingObjects/NetObjGenericSerializedClass.hpp>
#include <Helpers/NetworkingObjects/DelayUpdateSerializedClassManager.hpp>
#include "Subscription/TokenVerifier.hpp"
#include "AvatarStore.hpp"
#include "PublishedCanvases.hpp"
#include "MainProgram.hpp"
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include "DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "Helpers/NetworkingObjects/NetObjOrderedList.hpp"
#include "Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp"
#include "Helpers/NetworkingObjects/NetObjUnorderedSet.hpp"
#include "CommandList.hpp"
#include "Helpers/StringHelpers.hpp"
#include "MainProgram.hpp"
#include "SharedTypes.hpp"
#include "Toolbar.hpp"
#include "VersionConstants.hpp"
#include "WorldGrid.hpp"
#include <cereal/archives/portable_binary.hpp>
#include <chrono>
#include <include/core/SkFontMetrics.h>
#include <Helpers/Networking/NetLibrary.hpp>
#include <Helpers/Logger.hpp>
#include <cereal/types/vector.hpp>
#include <Helpers/Networking/NetLibrary.hpp>
#include <zstd.h>
#include <fstream>
#include <memory>
#include "CanvasComponents/CanvasComponent.hpp"
#include "CanvasComponents/CanvasComponentContainer.hpp"
#include "CanvasComponents/CanvasComponentAllocator.hpp"
#include "WorldScreenshot.hpp"
#include <Helpers/Random.hpp>

#ifdef _WIN32
#include <cstdio>
#include <windows.h>
namespace {
// Wraps a no-arg callable in a Windows structured-exception (SEH) guard so
// access violations during cereal serialization don't terminate the process.
// Returns 0 on success or the SEH exception code on failure. The handler
// body intentionally contains no C++ objects with destructors — MSVC won't
// emit the right unwind tables if a function mixes SEH with C++ unwinding.
template <typename Fn>
unsigned long seh_guard_call(Fn&& fn) noexcept {
    __try {
        fn();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}
}
#endif

#ifdef __EMSCRIPTEN__
    #include <EmscriptenHelpers/emscripten_browser_file.h>
#endif

#ifndef __EMSCRIPTEN__
namespace {
// RAII: deletes a path on scope exit (success or exception). Used to clear the
// uncompressed serialization spill file no matter how the save path unwinds.
struct ScopedFileRemove {
    std::filesystem::path path;
    ~ScopedFileRemove() {
        if(!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

// Stream-compresses the uncompressed cereal file `srcPath` into `tmpPath`:
// writes the version header, then a ZSTD frame produced by reading `srcPath`
// in fixed-size chunks through ZSTD_compressStream2. Peak memory is just the
// two ~128KB I/O buffers + the zstd window, regardless of canvas size.
//
// Why read from a file instead of a memory buffer: a large canvas serializes
// to ~GBs, and holding that in a std::stringstream (which DOUBLES to grow)
// blew past available RAM with bad_alloc. Spilling the uncompressed stream to
// disk first removes the only remaining large allocation on the save path.
//
// `srcSize` is pledged so the frame embeds its content size in the header —
// World::load_from_file sizes its decompress buffer via ZSTD_getFrameContentSize
// (UNKNOWN for streamed frames unless pledged), so the load path is unchanged
// and no save-format bump is needed.
//
// Throws std::runtime_error on any failure. The caller's atomic .tmp->rename
// finalization is unchanged.
void write_compressed_file_to_tmp(const std::filesystem::path& tmpPath,
                                  std::string_view header,
                                  const std::filesystem::path& srcPath,
                                  uint64_t srcSize) {
    std::ifstream in(srcPath, std::ios::binary);
    if(!in)
        throw std::runtime_error("Could not open uncompressed temp for reading: " + srcPath.string());
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if(!out)
        throw std::runtime_error("Could not open temp file for writing: " + tmpPath.string());

    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    if(!out)
        throw std::runtime_error("Failed to write header to " + tmpPath.string());

    std::unique_ptr<ZSTD_CCtx, size_t(*)(ZSTD_CCtx*)> cctx(ZSTD_createCCtx(), ZSTD_freeCCtx);
    if(!cctx)
        throw std::runtime_error("ZSTD_createCCtx failed");

    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    if(ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(cctx.get(), srcSize)))
        throw std::runtime_error("ZSTD_CCtx_setPledgedSrcSize failed");

    const size_t inCap = ZSTD_CStreamInSize();
    std::vector<char> inBuf(inCap);
    std::vector<char> outBuf(ZSTD_CStreamOutSize());

    uint64_t totalRead = 0;
    for(;;) {
        in.read(inBuf.data(), static_cast<std::streamsize>(inCap));
        const size_t got = static_cast<size_t>(in.gcount());
        totalRead += got;
        const bool lastChunk = (totalRead >= srcSize) || in.eof();
        const ZSTD_EndDirective mode = lastChunk ? ZSTD_e_end : ZSTD_e_continue;

        ZSTD_inBuffer zin{ inBuf.data(), got, 0 };
        for(;;) {
            ZSTD_outBuffer zout{ outBuf.data(), outBuf.size(), 0 };
            const size_t rem = ZSTD_compressStream2(cctx.get(), &zout, &zin, mode);
            if(ZSTD_isError(rem))
                throw std::runtime_error(std::string("ZSTD_compressStream2 failed: ") + ZSTD_getErrorName(rem));
            out.write(outBuf.data(), static_cast<std::streamsize>(zout.pos));
            if(!out)
                throw std::runtime_error("Failed to write compressed data to " + tmpPath.string());
            if(mode == ZSTD_e_end) { if(rem == 0) break; }      // frame fully flushed
            else if(zin.pos == zin.size) break;                 // chunk consumed
        }
        if(lastChunk) break;
    }
    if(in.bad())
        throw std::runtime_error("Read error on uncompressed temp: " + srcPath.string());

    out.flush();
    out.close();
    if(out.fail())
        throw std::runtime_error("Failed to finalize temp file: " + tmpPath.string());
}
}  // namespace
#endif

World::World(MainProgram& initMain, const CustomEvents::OpenInfiniPaintFileEvent& worldInfo):
    netObjMan(!worldInfo.isClient),
    main(initMain),
    undo(*this),
    rMan(*this),
    drawProg(*this),
    bMan(*this),
    wpGraph(*this),
    treeView(*this),
    readerMode(*this),
    gridMan(*this),
    canvasTheme(*this)
{
    saveThumbnail = worldInfo.saveThumbnail;
    autosaveSessionId = Random::get().alphanumeric_str(16);
    lastAutosaveAttemptTime = std::chrono::steady_clock::now();

    init_net_obj_type_list();
    netObjMan.set_netobj_destroy_callback([&undo = undo](const NetworkingObjects::NetObjID& idToDestroy) {
        undo.remove_by_netid(idToDestroy);
    });
    netObjMan.set_netid_reassign_callback([&undo = undo](const NetworkingObjects::NetObjID& oldID, const NetworkingObjects::NetObjID& newID) {
        undo.reassign_netid(oldID, newID);
    });

    netSource = worldInfo.netSource;

    drawData.rMan = &rMan;
    drawData.main = &main;
    drawData.cam.set_viewing_area(main.window.size.cast<float>());

    if(worldInfo.isClient)
        init_client(worldInfo.netSource, worldInfo.subscriberToken);
    else {
        init_client_data_list();
        set_name(name);
        if(worldInfo.filePathSource.has_value())
            load_from_file(worldInfo.filePathSource.value(), worldInfo.fileDataBuffer);
        else
            load_empty_canvas(worldInfo.filePathEmptyAutoSaveDir);
        #ifdef ENABLE_ORDERED_LIST_TEST
            listDebugTest = netObjMan.make_obj<NetworkingObjects::NetObjOrderedList<uint16_t>>();
        #endif
    }
}

void World::init_client_data_list() {
    clients = netObjMan.make_obj<NetworkingObjects::NetObjUnorderedSet<ClientData>>();
    // PHASE3 §4 B.M4 -- broadcast the local avatar with the rest of the
    // ClientData payload on first construction. Wire form (64x64 PNG)
    // is precomputed by AvatarStore::save so we don't re-downscale per
    // broadcast. Empty bytes when the artist has never captured one,
    // and receivers fall back to the colored-circle cursor.
    ownClientData = clients->emplace_direct(clients, ClientData::InitStruct{
        .cursorColor = get_random_cursor_color(),
        .displayName = main.conf.displayName,
        .avatarPng   = AvatarStore::load_wire_bytes(main.conf.configPath)
    });
    init_client_data_list_callbacks();
}

Vector3f World::get_random_cursor_color() {
    Vector3f hsv;
    hsv[0] = Random::get().real_range(0.0f, 360.0f);
    hsv[1] = Random::get().real_range(0.3f, 0.7f);
    hsv[2] = 1.0;
    return hsv_to_rgb<Vector3f>(hsv);
}

void World::init_client_data_list_callbacks() {
    clients->set_insert_callback([&](const NetworkingObjects::NetObjOwnerPtr<ClientData>& objPtr) {
        add_chat_message(objPtr->get_display_name(), "joined", Toolbar::ChatMessage::Type::JOIN);
    });
    clients->set_erase_callback([&](const NetworkingObjects::NetObjOwnerPtr<ClientData>& objPtr) {
        add_chat_message(objPtr->get_display_name(), "left", Toolbar::ChatMessage::Type::JOIN);
    });
}

void World::init_net_obj_type_list() {
    BookmarkListItem::register_class(*this);
    WaypointGraph::register_class(*this);
    WorldGrid::register_class(*this);
    NetworkingObjects::register_ordered_list_class<WorldGrid>(netObjMan);
    CanvasComponentAllocator::register_class(*this);
    CanvasComponentContainer::register_class(*this);
    NetworkingObjects::register_ordered_list_class<CanvasComponentContainer>(netObjMan);
    DrawingProgramLayerListItem::register_class(*this);
    NetworkingObjects::register_ordered_list_class<DrawingProgramLayerListItem>(netObjMan);
    canvasTheme.register_class();
    ClientData::register_class(*this);
    NetworkingObjects::register_unordered_set_class<ClientData>(netObjMan);

#ifdef ENABLE_ORDERED_LIST_TEST
    NetworkingObjects::register_generic_serialized_class<uint16_t>(netObjMan);
    NetworkingObjects::register_ordered_list_class<uint16_t>(netObjMan);
#endif
}

void World::init_client(const std::string& serverFullID, const std::string& subscriberToken) {
    main.init_net_library();
    clientStillConnecting = true;
    netClient = std::make_shared<NetClient>(serverFullID);
    lastKeepAliveSent = std::chrono::steady_clock::now();
    NetLibrary::register_client(netClient);
    netObjMan.set_client(netClient, SERVER_UPDATE_NETWORK_OBJECT, SERVER_UPDATE_MANY_NETWORK_OBJECTS);
    set_name("");

    rMan.init_client_callbacks();
    drawProg.init_client_callbacks();
    netClient->add_recv_callback(CLIENT_INITIAL_DATA, [&](cereal::PortableBinaryInputArchive& message) {
        std::string fileDisplayName;
        NetworkingObjects::NetObjID clientDataObjID;
        message(fileDisplayName, clientDataObjID);
        set_name(fileDisplayName);
        bMan.read_create_message(message);
        wpGraph.read_create_message(message);
        gridMan.read_create_message(message);
        drawProg.read_components_client(message);
        canvasTheme.read_create_message(message);
        clients = netObjMan.read_create_message<NetworkingObjects::NetObjUnorderedSet<ClientData>>(message, nullptr);
        init_client_data_list_callbacks();
        ownClientData = netObjMan.get_obj_temporary_ref_from_id<ClientData>(clientDataObjID);
        drawData.cam.smooth_move_to(*main.world, ownClientData->get_cam_coords(), ownClientData->get_window_size(), true);

        #ifdef ENABLE_ORDERED_LIST_TEST
            listDebugTest = netObjMan.read_create_message<NetworkingObjects::NetObjOrderedList<uint16_t>>(message, nullptr);
        #endif

        clientStillConnecting = false;
        set_to_layout_gui_if_focus();
    });
    netClient->add_recv_callback(CLIENT_UPDATE_NETWORK_OBJECT, [&](cereal::PortableBinaryInputArchive& message) {
        netObjMan.read_update_message(message, nullptr);
    });
    netClient->add_recv_callback(CLIENT_UPDATE_MANY_NETWORK_OBJECTS, [&](cereal::PortableBinaryInputArchive& message) {
        netObjMan.read_many_update_message(message, nullptr);
    });
    netClient->add_recv_callback(CLIENT_KEEP_ALIVE, [&](cereal::PortableBinaryInputArchive& message) {
    });

    // P0-C7 / P0-D2: handshake extended with subscriber token. Empty
    // string for vanilla collab joins; non-empty when the subscriber
    // pasted a token into the Connect-with-token dialog.
    // PHASE3 §4 B.M4 -- bundle the local avatar's wire bytes (64x64
    // PNG) into the initial handshake so the host can populate the new
    // ClientData with our avatar before broadcasting it to other peers.
    auto avatarWireBytes = AvatarStore::load_wire_bytes(main.conf.configPath);
    netClient->send_items_to_server(RELIABLE_COMMAND_CHANNEL, SERVER_INITIAL_DATA,
                                    main.conf.displayName, subscriberToken, avatarWireBytes);
}

void World::focus_update() {
    RenderStats::Frame& rs = RenderStats::get().live;   // PHASE5.5 M0 phase timing
    const auto focusStart = std::chrono::steady_clock::now();

    if(!clientStillConnecting) {
        delayedUpdateObjectManager.update(netObjMan);
        constexpr float SECONDS_TO_SEND_CAMERA_DATA = 0.5f;
        timeToSendCameraData.update_time_since();
        if(timeToSendCameraData.get_time_since() > SECONDS_TO_SEND_CAMERA_DATA) {
            timeToSendCameraData.update_time_point();
            ownClientData->set_window_size(ownClientData, main.window.size.cast<float>().eval());
            ownClientData->set_camera_coords(ownClientData, drawData.cam.c);
        }
        ownClientData->set_cursor_pos(ownClientData, main.input.mouse.pos);
        drawProg.update();
        #ifdef ENABLE_ORDERED_LIST_TEST
            list_debug_test_update();
        #endif
    }

    const auto camStart = std::chrono::steady_clock::now();
    drawData.cam.update_main(*this);
    rs.camUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - camStart).count();

    // TRANSITIONS.md T7 — drives the auto-advance state machine
    // for transition points. No-op when reader mode isn't active or
    // we aren't on a transition. Placed after the camera tick so the
    // local arrival-timer and the actual camera move use the same
    // deltaTime in the same frame ordering.
    const auto readerStart = std::chrono::steady_clock::now();
    readerMode.update(main.deltaTime);
    rs.readerUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readerStart).count();

    // PHASE10 Feature B — advance flip-book groups. Placed after the camera tick
    // so the AUTO on-screen trigger reads the current frame's view collider.
    drawProg.update_flipbook_playback(main.deltaTime);

    const auto rManStart = std::chrono::steady_clock::now();
    rMan.update();
    rs.rManUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rManStart).count();

    rs.focusUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - focusStart).count();
}

bool World::connection_update() {
    if(netServer) {
        if(netServer->is_disconnected()) {
            Logger::get().log("USERINFO", "Host connection failed");
            netSource.clear();
            netServer = nullptr;
            netObjMan.disconnect();
        }
        else {
            netServer->update();
            if(std::chrono::steady_clock::now() - lastKeepAliveSent > std::chrono::seconds(2)) {
                netServer->send_items_to_all_clients(UNRELIABLE_COMMAND_CHANNEL, CLIENT_KEEP_ALIVE);
                lastKeepAliveSent = std::chrono::steady_clock::now();
            }
        }
    }
    else if(netClient) {
        if(netClient->is_disconnected()) {
            Logger::get().log("USERINFO", "Client connection failed");
            netObjMan.disconnect();
            main.set_tab_to_close(this);
            return false;
        }
        netClient->update();
        if(std::chrono::steady_clock::now() - lastKeepAliveSent > std::chrono::seconds(2)) {
            netClient->send_items_to_server(UNRELIABLE_COMMAND_CHANNEL, SERVER_KEEP_ALIVE);
            lastKeepAliveSent = std::chrono::steady_clock::now();
        }
    }
    return true;
}

bool World::is_focus() {
    return main.world.get() == this;
}

void World::set_to_layout_gui_if_focus() {
    if(is_focus())
        main.g.gui.set_to_layout();
}

void World::undo_with_checks() {
    if(!clientStillConnecting && !drawProg.prevent_undo_or_redo())
        undo.undo();
}

void World::redo_with_checks() {
    if(!clientStillConnecting && !drawProg.prevent_undo_or_redo())
        undo.redo();
}

void World::update() {
    connection_update();
    autosave_tick();
}

void World::on_tab_out() {
    rMan.clear_display_cache();
    if(!clientStillConnecting)
        drawProg.on_tab_out();
}

void World::input_add_file_to_canvas_callback(const CustomEvents::AddFileToCanvasEvent& addFile) {
    if(!clientStillConnecting)
        drawProg.input_add_file_to_canvas_callback(addFile);
}

void World::input_paste_callback(const CustomEvents::PasteEvent& paste) {
    if(!clientStillConnecting)
        drawProg.input_paste_callback(paste);
}

void World::input_text_key_callback(const InputManager::KeyCallbackArgs& key) {
    // Reader-mode navigation also hijacks arrow keys when a text input
    // happens to be focused — otherwise an editor session ending while
    // a text box still has focus would silently swallow the arrows.
    if(readerMode.is_active() && key.down && !key.repeat) {
        if(key.key == InputManager::KEY_GENERIC_RIGHT) { readerMode.forward(); return; }
        if(key.key == InputManager::KEY_GENERIC_LEFT)  { readerMode.back();    return; }
    }
    if(!clientStillConnecting)
        drawProg.input_text_key_callback(key);
}

void World::input_text_callback(const InputManager::TextCallbackArgs& text) {
    if(!clientStillConnecting)
        drawProg.input_text_callback(text);
}

void World::input_drop_file_callback(const InputManager::DropCallbackArgs& drop) {
    if(!clientStillConnecting)
        drawProg.input_drop_file_callback(drop);
}

void World::input_drop_text_callback(const InputManager::DropCallbackArgs& drop) {
    if(!clientStillConnecting)
        drawProg.input_drop_text_callback(drop);
}

void World::input_key_callback(const InputManager::KeyCallbackArgs& key) {
    if(!clientStillConnecting) {
        // Reader-mode navigation intercepts arrow keys ahead of the rest
        // of the input chain. Arrow keys are dispatched as the unassignable
        // KEY_GENERIC_LEFT/RIGHT codes (InputManager.cpp special-cases
        // them outside the keyAssignments map, so a custom
        // KEY_READER_FORWARD enum + binding wouldn't fire).
        if(readerMode.is_active() && key.down && !key.repeat) {
            if(key.key == InputManager::KEY_GENERIC_RIGHT) { readerMode.forward(); return; }
            if(key.key == InputManager::KEY_GENERIC_LEFT)  { readerMode.back();    return; }
        }
        switch(key.key) {
            case InputManager::KEY_REDO: {
                if(key.down)
                    redo_with_checks();
                break;
            }
            case InputManager::KEY_UNDO: {
                if(key.down)
                    undo_with_checks();
                break;
            }
        }
        drawProg.input_key_callback(key);
        drawData.cam.input_key_callback(key);
    }
}

void World::input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if(!clientStillConnecting) {
        // In reader mode the canvas-side tools shouldn't react to mouse
        // input — only the GUI overlay (eye toggle, branch-choice
        // buttons) should. Otherwise a click on a floating button can
        // also be picked up by e.g. WaypointTool's marker-focus logic
        // and silently navigate the camera without updating ReaderMode
        // state.
        if(readerMode.is_active()) {
            // Reader mode ignores canvas tools, but a tap can still trigger
            // PARTICLE_PLAY_ON_TOUCH effects (PHASE5.1) and ON_TOUCH flip-book
            // groups (PHASE10 Feature B) under the cursor.
            if(button.down) {
                drawProg.trigger_touch_particles(button.pos);
                drawProg.trigger_touch_flipbooks(button.pos);
            }
            return;
        }
        drawProg.input_mouse_button_callback(button);
        drawData.cam.input_mouse_button_on_canvas_callback(*this, button);
    }
}

void World::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(!clientStillConnecting) {
        drawProg.input_mouse_motion_callback(motion);
        drawData.cam.input_mouse_motion_callback(*this, motion);
    }
}

void World::input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& wheel) {
    if(!clientStillConnecting)
        drawData.cam.input_mouse_wheel_callback(*this, wheel);
}

void World::input_pen_button_callback(const InputManager::PenButtonCallbackArgs& button) {
    if(!clientStillConnecting)
        drawProg.input_pen_button_callback(button);
}

void World::input_pen_touch_callback(const InputManager::PenTouchCallbackArgs& touch) {
    if(!clientStillConnecting)
        drawProg.input_pen_touch_callback(touch);
}

void World::input_pen_motion_callback(const InputManager::PenMotionCallbackArgs& motion) {
    if(!clientStillConnecting)
        drawProg.input_pen_motion_callback(motion);
}

void World::input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis) {
    if(!clientStillConnecting)
        drawProg.input_pen_axis_callback(axis);
}

void World::input_multi_finger_touch_callback(const InputManager::MultiFingerTouchCallbackArgs& touch) {
    if(!clientStillConnecting)
        drawData.cam.input_multi_finger_touch_callback(*this, touch);
}

void World::input_multi_finger_motion_callback(const InputManager::MultiFingerMotionCallbackArgs& motion) {
    if(!clientStillConnecting)
        drawData.cam.input_multi_finger_motion_callback(*this, motion);
}

std::optional<InputManager::TextBoxStartInfo> World::get_text_box_start_info() {
    if(!clientStillConnecting)
        return drawProg.get_text_box_start_info();
    return std::nullopt;
}

void World::send_chat_message(const std::string& message) {
    if(!clientStillConnecting)
        ownClientData->send_chat_message(ownClientData, *this, message);
}

void World::add_chat_message(const std::string& name, const std::string& message, Toolbar::ChatMessage::Type type) {
    Logger::get().log("CHAT", type == Toolbar::ChatMessage::JOIN ? (name + " " + message) : ("[" + name + "] " + message));
    chatMessages.emplace_front(Toolbar::ChatMessage{name, message, type});
    if(chatMessages.size() == CHAT_SIZE)
        chatMessages.pop_back();
    set_to_layout_gui_if_focus();
}

void World::early_destroy() {
    //con.early_destroy();
}

void World::set_name(const std::string& n) {
    if(n.empty())
        name = "New File";
    else
        name = n;
}

void World::ensure_display_name_unique(std::string& displayName) {
    std::vector<std::string> strList;
    for(auto& client : clients->get_data())
        strList.emplace_back(client->get_display_name());
    displayName = ensure_string_unique(strList, displayName);
}

void World::start_hosting(HostMode mode, const std::string& initNetSource, const std::string& serverLocalID) {
    hostMode = mode;

    // Dev-mode auto-publish for SUBSCRIPTION hosting when the file has
    // no portal metadata yet — populate from dev keys so dev-minted
    // tokens validate. Production artists publish via the portal first,
    // which writes the metadata into the .inkternity file and makes
    // SUBSCRIPTION the natural default at host time.
    if (hostMode == HostMode::SUBSCRIPTION && !has_subscription_metadata() &&
        !main.devKeys.member_pubkey().empty() &&
        !main.devKeys.canvas_id().empty() &&
        !main.devKeys.app_pubkey().empty()) {
        canvasId            = main.devKeys.canvas_id();
        artistMemberPubkey  = main.devKeys.member_pubkey();
        appPubkeyAtPublish  = main.devKeys.app_pubkey();
        Logger::get().log("USERINFO",
            "DevKeys: populated subscription metadata for SUBSCRIPTION host (canvas " + canvasId + ")");
    }

    // Defensive: if the caller asked for SUBSCRIPTION but no metadata
    // is available (UI grey-out failed, or programmatic caller), degrade
    // to COLLAB rather than running a token-check that can never pass.
    if (hostMode == HostMode::SUBSCRIPTION && !has_subscription_metadata()) {
        Logger::get().log("USERINFO",
            "SUBSCRIPTION host requested but canvas has no subscription metadata — falling back to COLLAB");
        hostMode = HostMode::COLLAB;
    }

    // SUBSCRIPTION hosting derives BOTH halves of the lobby address from
    // (app_secret, canvas_id) via HMAC-SHA-512/256 so the artist's share
    // code is stable across sessions and (with Phase 1 key restoration)
    // across reinstalls — see DISTRIBUTION-PHASE0.md §12.5. We install
    // the derived globalID on NetLibrary BEFORE init_net_library() so the
    // WSS connection opens on the stable path instead of the random one
    // get_global_id() would otherwise lazily generate.
    //
    // The HOST_MENU UI should already have computed and passed the same
    // value via initNetSource, but we recompute here too so programmatic
    // or UI-bypass callers can't accidentally start a SUBSCRIPTION session
    // on a random address that would invalidate every subscriber's saved
    // URL. Missing app_secret falls through to the random path (degraded
    // behavior, logged) rather than aborting the host attempt.
    if (hostMode == HostMode::SUBSCRIPTION) {
        if (main.devKeys.is_loaded()) {
            const auto derivedGlobal = CanvasShareId::derive_global_id(
                main.devKeys.app_seed_bytes(), canvasId);
            if (!derivedGlobal.empty()) {
                NetLibrary::set_global_id(derivedGlobal);
            } else {
                Logger::get().log("USERINFO",
                    "SUBSCRIPTION host: derive_global_id returned empty — "
                    "falling back to random globalID; share code WILL change across launches");
            }
        } else {
            Logger::get().log("USERINFO",
                "SUBSCRIPTION host: no app keypair available — falling back to random globalID; "
                "share code WILL change across launches");
        }
    }

    main.init_net_library();

    std::string effectiveLocalID = serverLocalID;
    std::string effectiveNetSource = initNetSource;
    if (hostMode == HostMode::SUBSCRIPTION) {
        effectiveLocalID = NetLibrary::deterministic_local_id_from_seed(canvasId);
        if (main.devKeys.is_loaded()) {
            const auto derivedLocal = CanvasShareId::derive_local_id(
                main.devKeys.app_seed_bytes(), canvasId);
            if (!derivedLocal.empty()) effectiveLocalID = derivedLocal;
        }
        effectiveNetSource = NetLibrary::get_global_id() + effectiveLocalID;
    }

    netServer = std::make_shared<NetServer>(effectiveLocalID);
    lastKeepAliveSent = std::chrono::steady_clock::now();
    NetLibrary::register_server(netServer);
    netObjMan.set_server(netServer, CLIENT_UPDATE_NETWORK_OBJECT, CLIENT_UPDATE_MANY_NETWORK_OBJECTS);
    netSource = effectiveNetSource;
    rMan.init_server_callbacks();
    drawProg.init_server_callbacks();
    netServer->add_recv_callback(SERVER_INITIAL_DATA, [&](std::shared_ptr<NetServer::ClientData> client, cereal::PortableBinaryInputArchive& message) {
        ClientData::InitStruct newClientData;
        newClientData.cursorColor = get_random_cursor_color();
        std::string subscriberToken;
        message(newClientData.displayName, subscriberToken, newClientData.avatarPng);
        ensure_display_name_unique(newClientData.displayName);

        // In SUBSCRIPTION host mode, run the five-check token verification
        // (TokenVerifier). Reject the connection cleanly on failure (the
        // subscriber sees a disconnect and an INVALID-token log on the
        // host). COLLAB mode skips this entirely — anyone with the lobby
        // address joins as a full collaborator.
        if (hostMode == HostMode::SUBSCRIPTION) {
            Subscription::TokenPayload payload;
            const auto r = Subscription::verify_token_for_host(subscriberToken, *this, payload);
            if (r != Subscription::VerifyResult::OK) {
                Logger::get().log("USERINFO",
                    "Rejected subscriber connect from " + newClientData.displayName +
                    ": " + Subscription::verify_result_str(r));
                client->setToDisconnect = true;
                return;
            }
            newClientData.isViewer = true;
            Logger::get().log("USERINFO",
                "Accepted subscriber " + newClientData.displayName +
                " (token sub=" + payload.sub.substr(0, 8) + "...)");
        }

        newClientData.camCoords = ownClientData->get_cam_coords();
        newClientData.windowSize = ownClientData->get_window_size();
        newClientData.gridSize = ownClientData->get_grid_size();

        NetworkingObjects::NetObjTemporaryPtr<ClientData> clientDataObjPtr = clients->emplace_direct(clients, newClientData);
        client->customID = clientDataObjPtr.get_net_id().data;
        auto ss(std::make_shared<std::stringstream>());
        {
            cereal::PortableBinaryOutputArchive a(*ss);
            a(CLIENT_INITIAL_DATA, name, clientDataObjPtr.get_net_id());
            bMan.bookmarkListRoot.write_create_message(a);
            wpGraph.write_create_message(a);
            gridMan.grids.write_create_message(a);
            drawProg.write_components_server(a);
            canvasTheme.write_create_message(a);
            clients.write_create_message(a);
            #ifdef ENABLE_ORDERED_LIST_TEST
                listDebugTest.write_create_message(a);
            #endif
        }
        netServer->send_string_stream_to_client(client, RELIABLE_COMMAND_CHANNEL, ss);
        for(auto& r : rMan.resource_list()) {
            netServer->send_items_to_client(client, RESOURCE_COMMAND_CHANNEL, CLIENT_NEW_RESOURCE_ID, r.get_net_id());
            netServer->send_items_to_client(client, RESOURCE_COMMAND_CHANNEL, CLIENT_NEW_RESOURCE_DATA, *r);
        }
    });
    netServer->add_recv_callback(SERVER_UPDATE_NETWORK_OBJECT, [&](std::shared_ptr<NetServer::ClientData> client, cereal::PortableBinaryInputArchive& message) {
        // Viewer gate: allow self-targeted ClientData updates (cursor,
        // camera, chat, display name) so presence/chat still work; drop
        // anything that mutates other NetObjs (strokes, layers, bookmarks,
        // canvas theme, etc.). The peek+dispatch split avoids re-reading
        // the ID inside NetObjManager.
        NetworkingObjects::NetObjID id;
        message(id);
        if(is_origin_viewer(client) && id.data != client->customID) return;
        netObjMan.dispatch_update_message(id, message, client);
    });
    netServer->add_recv_callback(SERVER_UPDATE_MANY_NETWORK_OBJECTS, [&](std::shared_ptr<NetServer::ClientData> client, cereal::PortableBinaryInputArchive& message) {
        // Multi-update batches are produced by canvas-mutation paths
        // (layer reorders, multi-component edits, bookmarks) — none of
        // which a legitimate viewer client triggers. Drop the whole
        // batch from a viewer rather than try to skip individual entries
        // (the wire format doesn't allow per-entry skipping without
        // running each readUpdateFunc, which would defeat the gate).
        if(is_origin_viewer(client)) return;
        netObjMan.read_many_update_message(message, client);
    });
    netServer->add_recv_callback(SERVER_KEEP_ALIVE, [&](std::shared_ptr<NetServer::ClientData> client, cereal::PortableBinaryInputArchive& message) {
    });
    netServer->add_disconnect_callback([&](std::shared_ptr<NetServer::ClientData> client) {
        NetworkingObjects::NetObjID idToErase;
        idToErase.data = client->customID;
        clients->erase(clients, idToErase);
    });
}

bool World::is_origin_viewer(const std::shared_ptr<NetServer::ClientData>& netClient) {
    // Only meaningful on the host. On a client, netServer is null and we
    // never receive these callbacks anyway. Defensive return = false so
    // a misuse on the client side fails open to the local user (their
    // own ops always apply locally — only the *host* enforces viewer-ness).
    if(!netClient) return false;
    auto clientData = netObjMan.get_obj_temporary_ref_from_id<ClientData>(
        NetworkingObjects::NetObjID(netClient->customID));
    if(!clientData) return false;  // entry already gone (mid-disconnect race)
    return clientData->is_viewer();
}

void World::autosave_to_directory(const std::filesystem::path& directoryToSaveAt) {
    std::vector<std::string> strList;
    // Glob both the canonical and legacy extensions so a stem already
    // taken by a .infpnt file isn't reused by a fresh .inkternity save.
    try {
        strList = glob_path_as_string_list(directoryToSaveAt, ("*" + World::DOT_FILE_EXTENSION).c_str(), 0, [&](const auto& p){ return p.stem().string();});
    }
    catch(...) {
    }
    try {
        auto legacy = glob_path_as_string_list(directoryToSaveAt, ("*" + World::LEGACY_DOT_FILE_EXTENSION).c_str(), 0, [&](const auto& p){ return p.stem().string();});
        strList.insert(strList.end(), legacy.begin(), legacy.end());
    }
    catch(...) {
    }
    std::string nameToSaveUnder = ensure_string_unique(strList, name);
    save_to_file(directoryToSaveAt / std::filesystem::path(nameToSaveUnder + "." + FILE_EXTENSION), true);
}

void World::serialize_canvas_to_stream(std::ostream& out) const {
    #ifdef _WIN32
        // SEH-guard the cereal recursion: an access violation deep in
        // serialization (e.g. a null member ptr) becomes a catchable
        // runtime_error instead of terminating the process and silently
        // losing the artist's work.
        std::string cppExMsg;
        const unsigned long sehCode = seh_guard_call([&] {
            try {
                cereal::PortableBinaryOutputArchive a(out);
                save_file(a);
            } catch (const std::exception& e) {
                cppExMsg = e.what();
                throw;
            }
        });
        if(sehCode != 0) {
            if(!cppExMsg.empty())
                throw std::runtime_error(
                    "Save failed: " + cppExMsg +
                    ". Your work is still in memory — try Save As again to a "
                    "different path.");
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%08lx", sehCode);
            throw std::runtime_error(
                std::string("Internal exception (code ") + buf +
                ") while serializing canvas. Your work is still in memory — "
                "try Save As again to a different path, or close any open tool "
                "dialogs and retry.");
        }
    #else
        cereal::PortableBinaryOutputArchive a(out);
        save_file(a);
    #endif
}

void World::save_to_file(const std::filesystem::path& filePathToSaveAtRaw, bool disableThumbnailSaving) {
    // The SDL save dialog on Windows doesn't auto-append the filter
    // extension when the artist types a bare name like "test" — the
    // returned path arrives extensionless and the saved file is then
    // invisible to subsequent Open dialogs filtered on .inkternity.
    // Normalize here so every save path (Save As, Save, autosave) lands
    // a properly-tagged file. Already-tagged paths pass through unchanged.
    std::filesystem::path filePathToSaveAt = filePathToSaveAtRaw;
    {
        const auto ext = filePathToSaveAt.extension().string();
        if(ext != DOT_FILE_EXTENSION && ext != LEGACY_DOT_FILE_EXTENSION)
            filePathToSaveAt += DOT_FILE_EXTENSION;
    }
    try {
        filePath = filePathToSaveAt;

        const std::string_view header(
            VersionConstants::CURRENT_SAVEFILE_HEADER.c_str(),
            VersionConstants::SAVEFILE_HEADER_LEN);

        #ifdef __EMSCRIPTEN__
            // Browser builds can't stream to a temp file; build the whole
            // compressed buffer in memory and hand it to the download helper.
            std::stringstream fWorldDataToCompress;
            serialize_canvas_to_stream(fWorldDataToCompress);
            set_name(filePath.stem().string());
            std::stringstream f;
            f.write(header.data(), header.size());
            std::vector<char> compressedData(ZSTD_compressBound(fWorldDataToCompress.view().size()));
            size_t trueCompressedSize = ZSTD_compress(compressedData.data(), compressedData.size(), fWorldDataToCompress.view().data(), fWorldDataToCompress.view().size(), ZSTD_CLEVEL_DEFAULT);
            f << std::string_view(compressedData.data(), trueCompressedSize);
            emscripten_browser_file::download(
                filePath.string(),
                "application/octet-stream",
                f.view()
            );
        #else
            // Atomic write: spill the uncompressed cereal stream to a temp
            // file, then stream-compress it into <path>.tmp and rename over
            // the target. Spilling to disk (instead of an in-memory buffer)
            // is what lets a multi-GB canvas save at all — a stringstream of
            // that size fails with bad_alloc as it doubles to grow. A crash
            // mid-write can't corrupt the prior good file at the target path.
            auto tmpPath = filePath;
            tmpPath += ".tmp";
            auto uncompressedTmp = filePath;
            uncompressedTmp += ".uncompressed.tmp";
            ScopedFileRemove spillGuard{uncompressedTmp};
            {
                std::ofstream uout(uncompressedTmp, std::ios::binary | std::ios::trunc);
                if(!uout)
                    throw std::runtime_error("Could not open temp file for writing: " + uncompressedTmp.string());
                serialize_canvas_to_stream(uout);
                uout.flush();
                if(!uout)
                    throw std::runtime_error("Failed writing serialized canvas to " + uncompressedTmp.string());
                uout.close();
                if(uout.fail())
                    throw std::runtime_error("Failed to finalize " + uncompressedTmp.string());
            }
            set_name(filePath.stem().string());
            const uint64_t uncompressedSize = std::filesystem::file_size(uncompressedTmp);
            write_compressed_file_to_tmp(tmpPath, header, uncompressedTmp, uncompressedSize);
            std::error_code rec;
            std::filesystem::rename(tmpPath, filePath, rec);
            if(rec) {
                // rename may fail across drives or on permission edge cases.
                // Fall back to copy + remove so the user's bytes still land
                // at the target path; if even copy fails, surface the tmp
                // location so the artist can recover manually.
                std::filesystem::copy_file(tmpPath, filePath,
                    std::filesystem::copy_options::overwrite_existing, rec);
                if(rec)
                    throw std::runtime_error(
                        "Could not finalize save at " + filePath.string() +
                        ": " + rec.message() +
                        " (canvas bytes remain at " + tmpPath.string() + ")");
                std::filesystem::remove(tmpPath, rec);
            }

            // C2PA: .inkternity sidecar signing is not supported.
            // c2pa-rs requires a recognized media type to compute hash
            // bindings; application/octet-stream is rejected. Image
            // exports (PNG/JPEG/WEBP) are signed via try_save_signed_image
            // in the screenshot/export path.

            if(saveThumbnail && !disableThumbnailSaving) {
                // Thumbnail is a JPG sidecar for the file-select tile.
                // Generation can fail transiently — e.g. take_screenshot_area_hw
                // returns "Error copy pixmap" if the GPU readback path is
                // momentarily unhappy. The canvas bytes already landed
                // atomically above, so this is a sidecar miss, not a save
                // failure. Swallow + log INFO so the artist doesn't see a
                // misleading WORLDFATAL toast; the next save will retry.
                try {
                    Vector2f imageCenter{main.window.size.x() * 0.5f, main.window.size.y() * 0.5f};
                    float imageDim = std::max(main.window.size.x(), main.window.size.y());
                    Vector2f imageDimVec{imageDim * 0.5f, imageDim * 0.5f};
                    SCollision::AABB<float> imageBounds{imageCenter - imageDimVec, imageCenter + imageDimVec};
                    world_take_screenshot(main.world, {
                        .filePath = filePath.parent_path() / (filePath.stem().string() + ".jpg"),
                        .type = WorldScreenshotInfo::ScreenshotType::JPG,
                        .imageSizePixels = {512, 512},
                        .cameraCoords = drawData.cam.c,
                        .imageBounds = imageBounds,
                        .transparentBackground = false,
                        .displayGrid = false
                    });
                }
                catch(const std::exception& e) {
                    Logger::get().log("INFO",
                        std::string("[save] thumbnail generation failed (canvas was saved): ") + e.what());
                }
            }
        #endif

        Logger::get().log("USERINFO", "File saved");
        undo.set_save_action();
        // The canvas just got persisted to the artist's chosen path, so
        // the autosave breadcrumb for this World is no longer protecting
        // anything. Removing it keeps the recovery list from accumulating
        // stale entries across long editing sessions.
        delete_autosave_file();
    }
    catch(const std::exception& e) {
        Logger::get().log("WORLDFATAL", std::string("Save error: ") + e.what());
    }
}

void World::save_recovery_copy(const std::filesystem::path& out) const {
    // Mirrors the atomic-write half of save_to_file / autosave_to_directory
    // (spill uncompressed -> stream-compress -> rename) but to an explicit
    // path, with NONE of save_to_file's stateful side effects. Safe to call
    // from the headless recovery path without disturbing the source file.
    const std::string_view header(
        VersionConstants::CURRENT_SAVEFILE_HEADER.c_str(),
        VersionConstants::SAVEFILE_HEADER_LEN);

    auto tmpPath = out;
    tmpPath += ".tmp";
    auto uncompressedTmp = out;
    uncompressedTmp += ".uncompressed.tmp";
    ScopedFileRemove spillGuard{uncompressedTmp};
    {
        std::ofstream uout(uncompressedTmp, std::ios::binary | std::ios::trunc);
        if(!uout)
            throw std::runtime_error("Could not open temp file for writing: " + uncompressedTmp.string());
        serialize_canvas_to_stream(uout);
        uout.flush();
        if(!uout)
            throw std::runtime_error("Failed writing serialized canvas to " + uncompressedTmp.string());
        uout.close();
        if(uout.fail())
            throw std::runtime_error("Failed to finalize " + uncompressedTmp.string());
    }
    const uint64_t uncompressedSize = std::filesystem::file_size(uncompressedTmp);
    write_compressed_file_to_tmp(tmpPath, header, uncompressedTmp, uncompressedSize);
    std::error_code rec;
    std::filesystem::rename(tmpPath, out, rec);
    if(rec) {
        std::filesystem::copy_file(tmpPath, out,
            std::filesystem::copy_options::overwrite_existing, rec);
        std::filesystem::remove(tmpPath, rec);
    }
}

std::filesystem::path World::autosave_file_path() const {
    return main.conf.configPath / "saves" / ".autosave" /
           (autosaveSessionId + DOT_FILE_EXTENSION);
}

void World::delete_autosave_file() {
    std::error_code ec;
    std::filesystem::remove(autosave_file_path(), ec);
}

void World::autosave_tick() {
#ifdef __EMSCRIPTEN__
    // Browser builds have no filesystem we can rely on for recovery;
    // the page-close confirmation already covers data-loss surface.
    return;
#else
    if(!hasUnsavedLocalChanges) return;
    if(clientStillConnecting) return;
    if(readerMode.is_active()) return;
    // Subscribers are read-only viewers — they don't own the canvas
    // state and don't need a recovery breadcrumb.
    if(ownClientData && ownClientData->is_viewer()) return;
    // In a collab/subscription client session, the host owns persistence;
    // the local copy is a live mirror, not the source of truth.
    if(netClient) return;

    const auto now = std::chrono::steady_clock::now();
    if(now - lastAutosaveAttemptTime < std::chrono::seconds(AUTOSAVE_INTERVAL_SECONDS))
        return;
    lastAutosaveAttemptTime = now;

    const auto path = autosave_file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if(ec) {
        Logger::get().log("INFO",
            "[autosave] could not create dir " + path.parent_path().string() +
            ": " + ec.message());
        return;
    }

    // We deliberately don't call save_to_file: it would mutate filePath /
    // name / set_save_action and treat this snapshot as the artist's
    // canonical save. Reproduce just the serialize + atomic-write parts
    // here, sharing serialize_canvas_to_stream + the file-spill streaming
    // compress with save_to_file's main path (so autosave also survives
    // multi-GB canvases instead of dying on a stringstream bad_alloc).
    try {
        const std::string_view header(
            VersionConstants::CURRENT_SAVEFILE_HEADER.c_str(),
            VersionConstants::SAVEFILE_HEADER_LEN);

        auto tmpPath = path;
        tmpPath += ".tmp";
        auto uncompressedTmp = path;
        uncompressedTmp += ".uncompressed.tmp";
        ScopedFileRemove spillGuard{uncompressedTmp};
        {
            std::ofstream uout(uncompressedTmp, std::ios::binary | std::ios::trunc);
            if(!uout)
                throw std::runtime_error("Could not open temp file for writing: " + uncompressedTmp.string());
            serialize_canvas_to_stream(uout);
            uout.flush();
            if(!uout)
                throw std::runtime_error("Failed writing serialized canvas to " + uncompressedTmp.string());
            uout.close();
            if(uout.fail())
                throw std::runtime_error("Failed to finalize " + uncompressedTmp.string());
        }
        const uint64_t uncompressedSize = std::filesystem::file_size(uncompressedTmp);
        write_compressed_file_to_tmp(tmpPath, header, uncompressedTmp, uncompressedSize);
        std::error_code rec;
        std::filesystem::rename(tmpPath, path, rec);
        if(rec) {
            std::filesystem::copy_file(tmpPath, path,
                std::filesystem::copy_options::overwrite_existing, rec);
            std::filesystem::remove(tmpPath, rec);
        }
    }
    catch(const std::exception& e) {
        Logger::get().log("INFO",
            std::string("[autosave] failed: ") + e.what());
    }
#endif
}

void World::load_empty_canvas(const std::optional<std::filesystem::path>& filePathEmptyAutoSaveDir) {
    gridMan.server_init_no_file();
    bMan.server_init_no_file();
    wpGraph.server_init_no_file();
    drawProg.server_init_no_file();
    canvasTheme.server_init_no_file();

    if(filePathEmptyAutoSaveDir.has_value())
        autosave_to_directory(filePathEmptyAutoSaveDir.value());
}

void World::load_from_file(const std::filesystem::path& filePathToLoadFrom, std::string_view buffer) {
    // If the artist opened this canvas from the autosave-recovery
    // directory, adopt the file's stem as our session id and clear
    // filePath. That way (a) subsequent autosaves overwrite the same
    // .autosave file instead of spawning a duplicate, (b) the cleanup
    // hook in save_to_file actually removes the recovered breadcrumb
    // when the artist finally saves to a real path, and (c) the canvas
    // is treated as never-saved-to-a-real-file, so File > Save will
    // open the Save As dialog as the artist would expect.
    const auto autosaveDir = main.conf.configPath / "saves" / ".autosave";
    std::error_code ec;
    if(std::filesystem::equivalent(filePathToLoadFrom.parent_path(), autosaveDir, ec)) {
        autosaveSessionId = filePathToLoadFrom.stem().string();
        filePath = std::filesystem::path();
    }
    else {
        filePath = filePathToLoadFrom;
    }

    std::string byteDataFromFile;

    if(buffer.empty()) {
        byteDataFromFile = read_file_to_string(filePathToLoadFrom);
        buffer = byteDataFromFile;
    }

    if(buffer.size() < VersionConstants::SAVEFILE_HEADER_LEN)
        throw std::runtime_error("[World::load_from_file] File is not an InfiniPaint canvas (File too small)");

    std::string_view fileHeader = buffer.substr(0, VersionConstants::SAVEFILE_HEADER_LEN);
    VersionNumber fileVersion = VersionConstants::header_to_version_number(std::string(fileHeader));
    std::string_view uncompressedDataView;
    std::vector<char> uncompressedDataVector;

    // RECOVERY: pad the decompressed buffer with trailing zeros so that a
    // tail over-read (load_file wanting a few more bytes than save wrote)
    // reads zeros instead of throwing on EOF. Gated to recoveryLoad so
    // normal loads keep their strict EOF behaviour. The per-subsystem
    // position log (below + in load_file) reveals whether the parse stayed
    // aligned to the very end (recovery trustworthy) or desynced early.
    const size_t recoveryPad = recoveryLoad ? size_t(1) << 16 : 0; // 64 KiB
    size_t trueUncompressedSize = 0;

    if(fileVersion < VersionNumber(0, 1, 0))
        uncompressedDataView = std::string_view(buffer.data() + VersionConstants::SAVEFILE_HEADER_LEN, buffer.size() - VersionConstants::SAVEFILE_HEADER_LEN);
    else {
        const size_t fcs = ZSTD_getFrameContentSize(buffer.data() + VersionConstants::SAVEFILE_HEADER_LEN, buffer.size() - VersionConstants::SAVEFILE_HEADER_LEN);
        uncompressedDataVector.assign(fcs + recoveryPad, '\0');
        trueUncompressedSize = ZSTD_decompress(uncompressedDataVector.data(), fcs, buffer.data() + VersionConstants::SAVEFILE_HEADER_LEN, buffer.size() - VersionConstants::SAVEFILE_HEADER_LEN);
        uncompressedDataView = std::string_view(uncompressedDataVector.data(), trueUncompressedSize + recoveryPad);
    }

    ByteMemStream f((char*)uncompressedDataView.data(), uncompressedDataView.size());
    if(recoveryLoad) {
        dbgLoadStream = &f;
        Logger::get().log("INFO", "[recover] decompressed " + std::to_string(trueUncompressedSize) +
            " bytes (+ " + std::to_string(recoveryPad) + " zero pad); beginning load_file");
    }

    Logger::get().log("INFO", "Loading file from version " + version_numbers_to_version_str(fileVersion));

    cereal::PortableBinaryInputArchive a(f);
    load_file(a, fileVersion);

    if(recoveryLoad) {
        const long long consumed = f.bytes_consumed();
        const long long over = consumed - static_cast<long long>(trueUncompressedSize);
        Logger::get().log("INFO", "[recover] load_file finished: consumed " + std::to_string(consumed) +
            " of " + std::to_string(trueUncompressedSize) + " real bytes (over-read into pad: " +
            std::to_string(over) + " byte(s))");
        dbgLoadStream = nullptr;
    }

    Logger::get().log("USERINFO", "File loaded");
    set_name(filePath.stem().string());
}

namespace {
    // .inkgroup container-body format. Bump if the body layout below changes
    // (this is independent of the canvas save-format version, which is stored
    // right after it and drives DrawingProgramLayerListItem::load_file gating).
    constexpr uint32_t GROUP_CONTAINER_FORMAT = 1;
}

void World::export_layer_group(const std::filesystem::path& pathRaw, const DrawingProgramLayerListItem& item) {
    std::filesystem::path path = pathRaw;
    if(path.extension().string() != DOT_GROUP_FILE_EXTENSION)
        path += DOT_GROUP_FILE_EXTENSION;

    // The layer blob references image/particle resources by NetObjID only; the
    // bytes live in ResourceManager and are NOT in item.save_file. Bundle the
    // ones this subtree uses so the group is self-contained across files.
    std::unordered_set<NetworkingObjects::NetObjID> usedResources;
    item.get_used_resources(usedResources);
    std::unordered_map<NetworkingObjects::NetObjID, ResourceData> resourceMap =
        rMan.copy_resource_set_to_map(usedResources);

    const std::string_view header(GROUP_FILE_HEADER.c_str(), VersionConstants::SAVEFILE_HEADER_LEN);

    // Spill the uncompressed body to a temp file, then stream-compress it and
    // rename over the target — same atomic-write path as save_to_file, so a
    // heavy multi-frame group survives instead of dying on an in-memory buffer.
    auto tmpPath = path;
    tmpPath += ".tmp";
    auto uncompressedTmp = path;
    uncompressedTmp += ".uncompressed.tmp";
    ScopedFileRemove spillGuard{uncompressedTmp};
    {
        std::ofstream uout(uncompressedTmp, std::ios::binary | std::ios::trunc);
        if(!uout)
            throw std::runtime_error("Could not open temp file for writing: " + uncompressedTmp.string());
        {
            cereal::PortableBinaryOutputArchive a(uout);
            a(GROUP_CONTAINER_FORMAT);
            // Canvas save-format version at export time — read back to gate
            // DrawingProgramLayerListItem::load_file exactly like a canvas load.
            a(VersionConstants::CURRENT_VERSION_NUMBER.major,
              VersionConstants::CURRENT_VERSION_NUMBER.minor,
              VersionConstants::CURRENT_VERSION_NUMBER.patch);
            item.save_file(a);
            a(resourceMap);
        }
        uout.flush();
        if(!uout)
            throw std::runtime_error("Failed writing layer group to " + uncompressedTmp.string());
        uout.close();
        if(uout.fail())
            throw std::runtime_error("Failed to finalize " + uncompressedTmp.string());
    }
    const uint64_t uncompressedSize = std::filesystem::file_size(uncompressedTmp);
    write_compressed_file_to_tmp(tmpPath, header, uncompressedTmp, uncompressedSize);
    std::error_code rec;
    std::filesystem::rename(tmpPath, path, rec);
    if(rec) {
        std::filesystem::copy_file(tmpPath, path,
            std::filesystem::copy_options::overwrite_existing, rec);
        if(rec)
            throw std::runtime_error(
                "Could not finalize layer group at " + path.string() + ": " + rec.message() +
                " (bytes remain at " + tmpPath.string() + ")");
        std::filesystem::remove(tmpPath, rec);
    }
    Logger::get().log("USERINFO", "Layer group exported");
}

DrawingProgramLayerListItem* World::read_layer_group_file(const std::filesystem::path& path) {
    std::string fileBytes = read_file_to_string(path);
    if(fileBytes.size() < VersionConstants::SAVEFILE_HEADER_LEN)
        throw std::runtime_error("[read_layer_group_file] Not a layer group file (too small)");

    const std::string_view fileHeader(fileBytes.data(), VersionConstants::SAVEFILE_HEADER_LEN);
    if(fileHeader != std::string_view(GROUP_FILE_HEADER.c_str(), VersionConstants::SAVEFILE_HEADER_LEN))
        throw std::runtime_error(
            "This is not a layer group (.inkgroup) file. Use Open to load a full canvas.");

    const char* payload = fileBytes.data() + VersionConstants::SAVEFILE_HEADER_LEN;
    const size_t payloadSize = fileBytes.size() - VersionConstants::SAVEFILE_HEADER_LEN;
    const unsigned long long fcs = ZSTD_getFrameContentSize(payload, payloadSize);
    if(fcs == ZSTD_CONTENTSIZE_ERROR || fcs == ZSTD_CONTENTSIZE_UNKNOWN)
        throw std::runtime_error("[read_layer_group_file] Corrupt layer group (bad compression frame)");
    std::vector<char> uncompressed(fcs, '\0');
    const size_t trueSize = ZSTD_decompress(uncompressed.data(), fcs, payload, payloadSize);
    if(ZSTD_isError(trueSize))
        throw std::runtime_error(std::string("[read_layer_group_file] Decompress failed: ") + ZSTD_getErrorName(trueSize));

    ByteMemStream f(uncompressed.data(), trueSize);
    cereal::PortableBinaryInputArchive a(f);

    uint32_t containerFormat = 0;
    a(containerFormat);
    if(containerFormat > GROUP_CONTAINER_FORMAT)
        throw std::runtime_error(
            "This layer group was made by a newer version of Inkternity. Update to import it.");

    VersionNumber version{0, 0, 0};
    a(version.major, version.minor, version.patch);

    // Build the subtree. load_file writes no net ids, so make_obj hands out
    // fresh random ids throughout — no collision with the destination canvas.
    auto item = std::make_unique<DrawingProgramLayerListItem>();
    item->load_file(a, version, drawProg.layerMan);

    std::unordered_map<NetworkingObjects::NetObjID, ResourceData> resourceMap;
    a(resourceMap);
    rMan.import_resource_map(resourceMap);

    return item.release();
}

void World::save_file(cereal::PortableBinaryOutputArchive& a) const {
    drawData.cam.save_file(a, *this);
    canvasTheme.save_file(a);
    drawProg.save_file(a);
    bMan.save_file(a);
    wpGraph.save_file(a);
    gridMan.save_file(a);
    rMan.save_file(a);
    // P0-C2: subscription metadata. Always written from format v0.11
    // onward; load gates on >= 0.11 so older files don't try to consume
    // bytes that aren't there.
    a(canvasId);
    a(artistMemberPubkey);
    a(appPubkeyAtPublish);
}

void World::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    // RECOVERY: log the stream position at the start of each subsystem so a
    // failed load points at the exact subsystem that over-reads, and a
    // padded successful load shows each subsystem consumed a sane amount.
    auto mark = [&](const char* what) {
        if(recoveryLoad)
            Logger::get().log("INFO", std::string("[recover]   @ ") +
                (dbgLoadStream ? std::to_string(dbgLoadStream->bytes_consumed()) : std::string("?")) +
                " -> " + what);
    };
    mark("cam");        drawData.cam.load_file(a, version, *this);
    mark("canvasTheme"); canvasTheme.load_file(a, version);
    mark("drawProg");   drawProg.load_file(a, version);
    mark("bMan");       bMan.load_file(a, version);
    if (version >= VersionNumber(0, 5, 0)) {
        mark("wpGraph"); wpGraph.load_file(a, version);
    }
    else
        wpGraph.server_init_no_file();  // M4-b will migrate from bMan instead
    mark("gridMan");    gridMan.load_file(a, version);
    mark("rMan");       rMan.load_file(a, version);
    if (version >= VersionNumber(0, 11, 0)) {
        mark("canvasId");          a(canvasId);
        mark("artistMemberPubkey"); a(artistMemberPubkey);
        mark("appPubkeyAtPublish"); a(appPubkeyAtPublish);
    }
    mark("done");
    // Older files default to empty (unpublished) — exactly what we want.
}

bool World::rename_on_disk(const std::string& newStem) {
    if (newStem.empty()) {
        Logger::get().log("WORLDFATAL",
            "Rename failed: new name is empty");
        return false;
    }
    if (filePath.empty()) {
        Logger::get().log("WORLDFATAL",
            "Rename failed: canvas hasn't been saved yet (use Save As)");
        return false;
    }
    // Filenames can't contain these on Windows + are generally a bad
    // idea on POSIX too. Reject up front rather than letting the move
    // partially succeed across a half-renamed sidecar set.
    static constexpr std::string_view forbidden = "<>:\"/\\|?*";
    if (newStem.find_first_of(forbidden) != std::string::npos) {
        Logger::get().log("WORLDFATAL",
            "Rename failed: name contains invalid characters (" +
            std::string(forbidden) + ")");
        return false;
    }
    const auto oldPath = filePath;
    if (oldPath.stem().string() == newStem) {
        // No-op rename — silently succeed.
        return true;
    }
    const auto newPath = oldPath.parent_path() /
                         (newStem + std::string(DOT_FILE_EXTENSION));
    std::error_code ec;
    if (std::filesystem::exists(newPath, ec)) {
        Logger::get().log("WORLDFATAL",
            "Rename failed: " + newPath.filename().string() +
            " already exists");
        return false;
    }

    // Stop any side-instance hosting this canvas under the old name so
    // the .lock file is released and the .inkternity file is no longer
    // open for write. No-op if nothing is managing this path.
    if (main.sideInstances) {
        main.sideInstances->stop(oldPath);
    }
    // If we (this foreground process) hold the lock, release it too.
    PublishedCanvases::release_lock(oldPath);

    // The canvas itself + every sidecar that lives at <oldPath>.<suffix>.
    // We rename whatever exists today; missing sidecars are silently
    // skipped (e.g. a never-published canvas has no .publish sidecar).
    const std::vector<std::string> sidecarSuffixes = {
        ".jpg",       // thumbnail (saved next to the canvas)
        ".publish",   // PublishedCanvases marker
        ".lock",      // PublishedCanvases PID lock (defensive — we
                      // released ours above, but a stale one from a
                      // crashed prior process could still be on disk)
    };

    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) {
        Logger::get().log("WORLDFATAL",
            "Rename failed for " + oldPath.filename().string() +
            " -> " + newPath.filename().string() + ": " + ec.message());
        return false;
    }
    for (const auto& suffix : sidecarSuffixes) {
        const auto oldSidecar = std::filesystem::path(
            oldPath.string() + suffix);
        if (!std::filesystem::exists(oldSidecar, ec)) continue;
        const auto newSidecar = std::filesystem::path(
            newPath.string() + suffix);
        std::filesystem::rename(oldSidecar, newSidecar, ec);
        if (ec) {
            // Non-fatal: the canvas file did move; only the sidecar
            // failed. Log and continue. Worst case the artist
            // re-publishes / a stale .lock gets reclaimed by stale-PID
            // detection.
            Logger::get().log("WORLDFATAL",
                "Sidecar " + oldSidecar.filename().string() +
                " could not move: " + ec.message());
            ec.clear();
        }
    }

    filePath = newPath;
    set_name(newStem);
    Logger::get().log("USERINFO",
        "Canvas renamed to " + newPath.filename().string());
    return true;
}

WorldScalar World::calculate_zoom_from_uniform_zoom(WorldScalar uniformZoom, WorldVec oldWindowSize) {
    WorldScalar a1(main.window.size.x() / static_cast<double>(oldWindowSize.x()));
    WorldScalar a2(main.window.size.y() / static_cast<double>(oldWindowSize.y()));
    return uniformZoom * ((a1 < a2) ? a1 : a2);
}

void World::draw_other_player_cursors(SkCanvas* canvas, const DrawData& drawData) {
    if(clients) {
        for(auto& clientDataPtr : clients->get_data()) {
            if(clientDataPtr != ownClientData)
                clientDataPtr->draw_cursor(canvas, drawData);
        }
    }
}

void World::scale_up_step() {
    if(ownClientData)
        ownClientData->scale_up_step(ownClientData, *this);
}

void World::scale_up(const WorldScalar& scaleUpAmount) {
    Logger::get().log("USERINFO", "Canvas scaled up");
    bMan.scale_up(scaleUpAmount);
    wpGraph.scale_up(scaleUpAmount);
    gridMan.scale_up(scaleUpAmount);
    drawData.cam.scale_up(*this, scaleUpAmount);
    // drawProg will be sending info on committed objects/modified grids. These objects will be scaled up already.
    // This means that the scale up message must be send BEFORE the World::scale_up function is called on our end
    // if this client is the one responsible for the scale up
    drawProg.scale_up(scaleUpAmount);
    undo.scale_up(scaleUpAmount);
}

bool World::should_ask_before_closing() {
    // If it's a client that didn't save to a file previously, we can ignore asking before quitting (probably don't intend to save anyway)
    // If it's a server or a client, and the previous statement is false, we should always ask before quitting because the data could be modified by someone on the network
    // If it's a local file, ask before quitting if we detect any local changes
    // If on web version, don't ask before closing
#ifdef __EMSCRIPTEN__
    return false;
#else
    if(netClient || netServer)
        return !(netClient && filePath.empty());
    return hasUnsavedLocalChanges;
#endif
}

void World::set_has_unsaved_local_changes(bool newHasUnsavedLocalChangesVal) {
    bool oldHasUnsavedLocalChanges = hasUnsavedLocalChanges;
    hasUnsavedLocalChanges = newHasUnsavedLocalChangesVal;
    if(oldHasUnsavedLocalChanges != hasUnsavedLocalChanges)
        main.g.gui.set_to_layout();
}

#ifdef ENABLE_ORDERED_LIST_TEST
void World::list_debug_test_update() {
    if(std::chrono::steady_clock::now() < (listDebugTestTimeStart + std::chrono::minutes(1))) {
        if(nextSendTime < std::chrono::steady_clock::now() - std::chrono::milliseconds(300)) {
            nextSendTime = std::chrono::steady_clock::now();

            bool isInsert;
            if(listDebugTest->size() > 70)
                isInsert = false;
            else if(listDebugTest->size() < 20)
                isInsert = true;
            else
                isInsert = Random::get().real_range(0.0f, 1.0f) > 0.5f;

            using namespace NetworkingObjects;
            if(isInsert) {
                std::vector<std::pair<NetObjOrderedListIterator<uint16_t>, NetObjOwnerPtr<uint16_t>>> toInsert;
                std::vector<uint32_t> randomIndices;
                uint32_t insertAmount = Random::get().int_range(5, 25);
                for(uint32_t i = 0; i < insertAmount; i++)
                    randomIndices.emplace_back(Random::get().int_range<uint32_t>(0, listDebugTest->size() + 10));
                randomIndices[1] = randomIndices[0];
                std::sort(randomIndices.begin(), randomIndices.end());
                for(uint32_t index : randomIndices)
                    toInsert.emplace_back(listDebugTest->at(index), netObjMan.make_obj_direct<uint16_t>(Random::get().int_range<uint32_t>(10, 100)));
                listDebugTest->insert_ordered_list_and_send_create(listDebugTest, toInsert);
            }
            else {
                std::vector<NetObjOrderedListIterator<uint16_t>> toErase;
                for(uint32_t i = 0; i < listDebugTest->size(); i++) {
                    if(Random::get().real_range(0.0f, 1.0f) > 0.8f)
                        toErase.emplace_back(listDebugTest->at(i));
                }
                listDebugTest->erase_list(listDebugTest, toErase);
            }
        }
    }
    if(netServer && netServer->get_client_list().empty())
        listDebugTestTimeStart = std::chrono::steady_clock::now();
}
#endif

void World::draw(SkCanvas* canvas, const DrawData& calledDrawData) {
    if(!clientStillConnecting) {
        if(calledDrawData.drawGrids)
            gridMan.draw_back(canvas, calledDrawData);
        drawProg.draw(canvas, calledDrawData);
        if(calledDrawData.drawGrids) {
            gridMan.draw_front(canvas, calledDrawData);
            gridMan.draw_coordinates(canvas, calledDrawData);
        }
        if(!calledDrawData.takingScreenshot)
            draw_other_player_cursors(canvas, calledDrawData);
    }
}
