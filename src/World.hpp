#pragma once
#include <Helpers/NetworkingObjects/DelayUpdateSerializedClassManager.hpp>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include "CoordSpaceHelper.hpp"
#include "CustomEvents.hpp"
#include "Helpers/NetworkingObjects/NetObjUnorderedSet.hpp"
#include "WorldUndoManager.hpp"
#include "Bookmarks/BookmarkManager.hpp"
#include "Waypoints/WaypointGraph.hpp"
#include "Waypoints/TreeView.hpp"
#include "ReaderMode/ReaderMode.hpp"
#include "ResourceManager.hpp"
#include "DrawingProgram/DrawingProgram.hpp"
#include "Toolbar.hpp"
#include "SharedTypes.hpp"
#include "CanvasTheme.hpp"
#include "GridManager.hpp"
#include <Helpers/NetworkingObjects/NetObjManager.hpp>
#include <chrono>
#include <filesystem>
#include <sstream>
#include "ClientData.hpp"
#include "HostMode.hpp"

class MainProgram;

//#define ENABLE_ORDERED_LIST_TEST

struct WorldScreenshotInfo;

class World {
    public:
        static constexpr std::string DOT_FILE_EXTENSION = ".inkternity";
        static constexpr std::string FILE_EXTENSION = "inkternity";
        // Pre-rebrand extension; readable for backward compat, never written.
        static constexpr std::string LEGACY_DOT_FILE_EXTENSION = ".infpnt";
        static constexpr size_t CHAT_SIZE = 10;

        World(MainProgram& initMain, const CustomEvents::OpenInfiniPaintFileEvent& worldInfo);

        // NOTE: Keep at the very beginning so that it's destroyed last
        NetworkingObjects::NetObjManager netObjMan;

        void save_file(cereal::PortableBinaryOutputArchive& a) const;
        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version);

        // Serialize the canvas into `out` as an uncompressed binary cereal
        // stream, SEH-guarded on Windows. Shared by save_to_file and
        // autosave_tick. `out` is typically an ofstream to a temp spill file
        // (desktop) or a stringstream (emscripten download). Throws
        // std::runtime_error if serialization faults; the caller decides
        // whether that is WORLDFATAL or a skipped autosave tick.
        void serialize_canvas_to_stream(std::ostream& out) const;

        MainProgram& main;
        DrawData drawData;
        WorldUndoManager undo;
        ResourceManager rMan;
        DrawingProgram drawProg;
        BookmarkManager bMan;
        WaypointGraph wpGraph;
        TreeView treeView;
        ReaderMode readerMode;
        GridManager gridMan;

        std::deque<Toolbar::ChatMessage> chatMessages;

        std::string netSource;

        // Two explicit hosting modes (DISTRIBUTION-PHASE0.md §C):
        //   COLLAB       — anyone with the lobby address can join with full
        //                  write access; no token check. Default for canvases
        //                  without portal subscription metadata.
        //   SUBSCRIPTION — every joiner must present a portal-issued (or
        //                  dev-minted) subscriber token that passes the five
        //                  checks in TokenVerifier; valid joiners are flagged
        //                  isViewer = true and the wire-side viewer gate
        //                  (World::is_origin_viewer) drops their write ops.
        // Mode is chosen at start_hosting time and cannot change mid-session.
        HostMode hostMode = HostMode::COLLAB;

        void start_hosting(HostMode mode, const std::string& initNetSource, const std::string& serverLocalID);

        void send_chat_message(const std::string& message);
        void add_chat_message(const std::string& name, const std::string& message, Toolbar::ChatMessage::Type type);

        WorldScalar calculate_zoom_from_uniform_zoom(WorldScalar uniformZoom, WorldVec oldWindowSize);

        void focus_update();
        void update();
        void on_tab_out();

        void draw(SkCanvas* canvas, const DrawData& calledDrawData);
        void early_destroy();

        bool clientStillConnecting = false;

        void autosave_to_directory(const std::filesystem::path& directoryToSaveAt);
        void save_to_file(const std::filesystem::path& filePathToSaveAt, bool disableThumbnailSaving = false);
        void load_from_file(const std::filesystem::path& filePathToLoadFrom, std::string_view buffer);

        // Periodic in-progress autosave. Called from World::update each
        // frame; cheap when there's nothing to do. Writes a snapshot to
        // autosave_file_path() every AUTOSAVE_INTERVAL_SECONDS when the
        // canvas has unsaved local changes, and silently no-ops in reader
        // mode, viewer mode, or while a client is mid-connect. Does NOT
        // mutate filePath / name / set_save_action — autosaves are a
        // recovery breadcrumb, not a "the artist saved" signal.
        void autosave_tick();

        // <configPath>/saves/.autosave/<autosaveSessionId>.inkternity.
        // Per-World id is generated in the constructor and is stable for
        // the World's lifetime so the autosave file gets overwritten in
        // place each tick. FileSelectScreen scans this dir on launch and
        // surfaces orphans as "Recovered Unsaved Work".
        std::filesystem::path autosave_file_path() const;

        // Remove the autosave file (if any) belonging to this World.
        // Called by save_to_file on a real-save success so the recovery
        // entry doesn't outlive the work it was protecting.
        void delete_autosave_file();

        // Rename the canvas file on disk to `newStem.inkternity` in the
        // same directory, carrying every sidecar (thumbnail, publish
        // marker, lock if held by us) with it. Updates filePath + name
        // on success. Coordinates with main.sideInstances to stop any
        // side-instance hosting the old path before the move, so the
        // lock file is released and the move can succeed.
        //
        // Returns true on success. Returns false on:
        //   - empty newStem
        //   - newStem same as current stem (no-op)
        //   - destination file already exists
        //   - filesystem error during the move
        // Caller is responsible for surfacing failure to the user; this
        // method only logs.
        bool rename_on_disk(const std::string& newStem);

        void undo_with_checks();
        void redo_with_checks();

        std::filesystem::path filePath;
        std::string name;

        // P0-C2 (DISTRIBUTION-PHASE0.md §C): subscription metadata.
        // Set when an artist publishes the canvas through the portal;
        // empty/default for unpublished canvases. The host-mode token
        // verifier reads these to validate incoming subscriber tokens
        // (canvas binding + identity binding). All three persisted in
        // the .inkternity file from format version 0.11 onward.
        //
        //   canvasId             — UUID assigned by the portal at
        //                          publish-for-subscribers time.
        //   artistMemberPubkey   — Stellar G... or hex pubkey of the
        //                          artist that owns this canvas. The
        //                          host's own member pubkey must match
        //                          this on every accepted token.
        //   appPubkeyAtPublish   — hex pubkey of the Inkternity app
        //                          install that published this canvas.
        //                          The host's own app pubkey must match.
        //
        // Subscription metadata. Eligibility for SUBSCRIPTION host mode
        // requires all three fields to be set (portal publish populates
        // them; dev-keys mode can substitute them at start_hosting time).
        std::string canvasId;
        std::string artistMemberPubkey;
        std::string appPubkeyAtPublish;
        bool has_subscription_metadata() const {
            return !canvasId.empty() && !artistMemberPubkey.empty() && !appPubkeyAtPublish.empty();
        }

        NetworkingObjects::NetObjTemporaryPtr<ClientData> ownClientData;
        NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjUnorderedSet<ClientData>> clients;

        CanvasTheme canvasTheme;

        void scale_up(const WorldScalar& scaleUpAmount);
        void scale_up_step();

        bool should_ask_before_closing();
        void set_has_unsaved_local_changes(bool newHasUnsavedLocalChangesVal);
        bool is_focus();
        void set_to_layout_gui_if_focus();

        NetworkingObjects::DelayUpdateSerializedClassManager delayedUpdateObjectManager;

        std::shared_ptr<NetServer> netServer;
        std::shared_ptr<NetClient> netClient;

        // Host-side viewer gate. Looks up the ClientData entry behind a
        // NetServer::ClientData via the customID stamped at handshake time
        // (World.cpp SERVER_INITIAL_DATA), and returns true iff that
        // entry's isViewer flag is set. Used by write-side recv callbacks
        // to silently drop mutation attempts from subscriber clients —
        // the client-side UI already hides the buttons, but a modified
        // client could still send the wire op, so the host must enforce.
        bool is_origin_viewer(const std::shared_ptr<NetServer::ClientData>& netClient);

        void input_add_file_to_canvas_callback(const CustomEvents::AddFileToCanvasEvent& addFile);
        void input_paste_callback(const CustomEvents::PasteEvent& paste);
        void input_drop_file_callback(const InputManager::DropCallbackArgs& drop);
        void input_drop_text_callback(const InputManager::DropCallbackArgs& drop);
        void input_text_key_callback(const InputManager::KeyCallbackArgs& key);
        void input_text_callback(const InputManager::TextCallbackArgs& text);
        void input_key_callback(const InputManager::KeyCallbackArgs& key);
        void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button);
        void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion);
        void input_mouse_wheel_callback(const InputManager::MouseWheelCallbackArgs& wheel);
        void input_pen_button_callback(const InputManager::PenButtonCallbackArgs& button);
        void input_pen_touch_callback(const InputManager::PenTouchCallbackArgs& touch);
        void input_pen_motion_callback(const InputManager::PenMotionCallbackArgs& motion);
        void input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis);
        void input_multi_finger_touch_callback(const InputManager::MultiFingerTouchCallbackArgs& touch);
        void input_multi_finger_motion_callback(const InputManager::MultiFingerMotionCallbackArgs& motion);
        std::optional<InputManager::TextBoxStartInfo> get_text_box_start_info();
    private:
        bool saveThumbnail = false;
        bool hasUnsavedLocalChanges = false;

        // Stable per-World id used as the autosave filename stem. Generated
        // in the constructor (random hex), so each open canvas gets its
        // own .autosave file and they don't collide. Persists for the
        // lifetime of the World object — not the canvas — so re-opening
        // the same file in a new session creates a fresh autosave id.
        std::string autosaveSessionId;
        std::chrono::steady_clock::time_point lastAutosaveAttemptTime{};
        static constexpr int AUTOSAVE_INTERVAL_SECONDS = 180; // 3 min

        void load_empty_canvas(const std::optional<std::filesystem::path>& filePathEmptyAutoSaveDir = std::nullopt);

        Vector3f get_random_cursor_color();
        void init_client_data_list();
        void init_client_data_list_callbacks();
        void init_net_obj_type_list();

        void init_client(const std::string& serverFullID, const std::string& subscriberToken);
        void set_name(const std::string& n);

        void draw_other_player_cursors(SkCanvas* canvas, const DrawData& drawData);
        void ensure_display_name_unique(std::string& displayName);

        bool connection_update();

        TimePoint timeToSendCameraData;

        std::chrono::steady_clock::time_point lastKeepAliveSent;

#ifdef ENABLE_ORDERED_LIST_TEST
        NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<uint16_t>> listDebugTest;
        void list_debug_test_update();
        std::chrono::steady_clock::time_point listDebugTestTimeStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point nextSendTime = std::chrono::steady_clock::now();
#endif
};
