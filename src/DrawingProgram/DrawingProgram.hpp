#pragma once
#include "../DrawData.hpp"
#include "DrawingProgramCache.hpp"
#include "ToolConfiguration.hpp"
#include "Tools/GridModifyTool.hpp"
#include <Helpers/NetworkingObjects/NetObjWeakPtr.hpp>
#include "Tools/PanCanvasTool.hpp"
#include "Tools/ZoomCanvasTool.hpp"
#include "cereal/archives/portable_binary.hpp"
#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <include/core/SkVertices.h>
#include <Helpers/SCollision.hpp>
#include <Helpers/Hashes.hpp>
#include <Helpers/Random.hpp>
#include "Tools/DrawingProgramToolBase.hpp"
#include <Helpers/FileDownloader.hpp>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include "Layers/DrawingProgramLayerManager.hpp"
#include "DrawingProgramSelection.hpp"
#include <memory>

class World;
class FxLibraryStore;
class LegacyFxLibrary;
class PhoneDrawingProgramScreen;
class DrawingProgram;
class DrawingProgramLayerListItem;
namespace RasterFlatten {
    void flatten_layer(DrawingProgram& drawP);
    bool merge_down_baked(DrawingProgram& drawP, DrawingProgramLayerListItem& upper, DrawingProgramLayerListItem& lower);
    void consolidate_vectors(DrawingProgram& drawP);
    void optimize_vectors(DrawingProgram& drawP);
}
namespace RasterResolution { void halve_layer(DrawingProgram& drawP); }

class DrawingProgram {
    public:
        DrawingProgram(World& initWorld);
        ~DrawingProgram();   // out-of-line for unique_ptr<FxLibraryStore>
        void server_init_no_file();
        void toolbar_gui(Toolbar& t);
        void tool_options_gui(Toolbar& t);
        void right_click_popup_gui(Toolbar& t);
        void update();
        void scale_up(const WorldScalar& scaleUpAmount);
        void draw(SkCanvas* canvas, const DrawData& drawData);
        // PHASE7: author-mode overlay outlining mask shapes (red dashed). Drawn
        // after the cached composite so it is NOT baked into the cache / flatten /
        // screenshots, and skipped in reader mode (editing aid only).
        void draw_mask_outlines(SkCanvas* canvas, const DrawData& drawData);
        // While an AI reangle/mesh request is in flight, dim the canvas and show a
        // centred "Building…" card so the wait reads as a deliberate modal state, not
        // a freeze. Drawn last (over everything, screen space), skipped in
        // screenshots/exports. Canvas input is blocked while the modal is up (see
        // ai_busy_modal_active); Esc drops it to a small corner pill (the request
        // keeps running and still places its result) so a slow cold start can't trap
        // the artist.
        void draw_ai_busy_overlay(SkCanvas* canvas, const DrawData& drawData);
        // True while an AI request is in flight AND the artist has not pressed Esc to
        // keep working. Canvas/tool input callbacks early-out on this.
        bool ai_busy_modal_active() const;
        void write_components_server(cereal::PortableBinaryOutputArchive& a);
        void read_components_client(cereal::PortableBinaryInputArchive& a);
        void init_server_callbacks();
        void init_client_callbacks();
        void add_file_to_canvas_by_path(const std::filesystem::path& filePath, Vector2f dropPos);
        CanvasComponentContainer::ObjInfo* add_file_to_canvas_by_data(const std::string& fileName, std::string_view fileBuffer, Vector2f dropPos);

        // AUDIO.md §4 — drop-and-attach path. Called from
        // add_file_to_canvas_by_path when an mp3 is dropped while the
        // WaypointTool is active and a waypoint is selected; bypasses
        // the ImageCanvasComponent creation, registers the mp3 bytes
        // as a ResourceData, sets the selected waypoint's audioId,
        // broadcasts the change. Enforces the 30 MB cumulative budget
        // (returns false without registering if the file would exceed).
        // Returns true if the audio was attached (drop should not fall
        // through to image-creation); false if the caller should
        // handle the file the normal way.
        bool try_attach_audio_to_selected_waypoint(const std::filesystem::path& filePath);
        // PHASE5 — drop a .tfx particle package onto the canvas to spawn a
        // ParticleCanvasComponent. Intercepts before the image-drop path;
        // returns true if it consumed the file.
        bool try_add_particle_effect(const std::filesystem::path& filePath, Vector2f dropPos);
        // PHASE5.1 M0 — import a legacy TimelineFX .eff library: parse + validate,
        // then embed its bytes as a canvas asset (ResourceManager). Host-gated.
        // No-op unless HVYM_HAS_TIMELINEFX_LEGACY.
        void import_fx_library(const std::filesystem::path& filePath);
        // PHASE5.1 polish — trigger any PARTICLE_PLAY_ON_TOUCH effects under a
        // cam-space point (reader-mode tap). No-op without the legacy path.
        void trigger_touch_particles(Vector2f camPos);
        // PHASE10 Feature B — per-frame flip-book playback tick (called from
        // World::focus_update) + ON_TOUCH dispatch for a reader-mode tap.
        void update_flipbook_playback(float deltaTime);
        void trigger_touch_flipbooks(Vector2f camPos);
#ifdef HVYM_HAS_TIMELINEFX_LEGACY
        // Imported FX libraries (M1). null until the first successful import.
        FxLibraryStore* fx_store() { return fxStore.get(); }
        std::unique_ptr<FxLibraryStore> fxStore;
        // M3 — resolve a parsed library by its embedded-resource id: from the
        // store, else parsed from the ResourceManager asset (cached in store).
        LegacyFxLibrary* resolve_fx_library(const NetworkingObjects::NetObjID& resourceId);
#endif
        void get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet);

        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version);
        void save_file(cereal::PortableBinaryOutputArchive& a) const;
        World& world;

        bool prevent_undo_or_redo();

        DrawingProgramCache drawCache;
        DrawingProgramLayerManager layerMan;

        Vector4f* get_foreground_color_ptr();
        void switch_to_tool(DrawingProgramToolType newToolType, bool force = false);
        void switch_to_tool_ptr(std::unique_ptr<DrawingProgramToolBase> newTool);
        // Duplicate the current selection in place (toolbar/menu action). No-op if
        // nothing is selected. Public wrapper over the private selection.
        void duplicate_selection();
        void modify_grid(const NetworkingObjects::NetObjWeakPtr<WorldGrid>& gridToModify);

        void invalidate_cache_at_component(CanvasComponentContainer::ObjInfo* objToCheck);
        void preupdate_component(CanvasComponentContainer::ObjInfo* objToCheck);
        void send_transforms_for(const std::vector<CanvasComponentContainer::ObjInfo*>& objsToSendTransformsFor);

        void on_tab_out();
        void input_add_file_to_canvas_callback(const CustomEvents::AddFileToCanvasEvent& addFile);
        void input_paste_callback(const CustomEvents::PasteEvent& paste);
        void input_drop_text_callback(const InputManager::DropCallbackArgs& drop);
        void input_drop_file_callback(const InputManager::DropCallbackArgs& drop);
        void input_text_key_callback(const InputManager::KeyCallbackArgs& key);
        void input_text_callback(const InputManager::TextCallbackArgs& text);
        void input_key_callback(const InputManager::KeyCallbackArgs& key);
        void input_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button);
        void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion);
        void input_pure_mouse_button_callback(const InputManager::MouseButtonCallbackArgs& button);
        void input_pure_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion);
        void input_pen_button_callback(const InputManager::PenButtonCallbackArgs& button);
        void input_pen_touch_callback(const InputManager::PenTouchCallbackArgs& touch);
        void input_pen_motion_callback(const InputManager::PenMotionCallbackArgs& motion);
        void input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis);
        std::optional<InputManager::TextBoxStartInfo> get_text_box_start_info();

        void set_right_click_popup_location(const Vector2f& newLoc);
        void clear_right_click_popup();

        std::unique_ptr<DrawingProgramToolBase> drawTool;
    private:
        void process_transform_message(const std::vector<std::pair<NetworkingObjects::NetObjID, CoordSpaceHelper>>& transforms);

        void drag_drop_update();
        void check_updateable_components();
        void update_downloading_dropped_files();

        void selection_action_menu(Vector2f popupPos);
        void right_click_action_menu(Vector2f popupPos, const std::function<void()>& innerContent);
        void popup_menu_action_button(const char* id, const char* text, const std::function<void()>& onClick);
        void rebuild_cache();

        float drag_point_radius();
        void draw_drag_circle(SkCanvas* canvas, const Vector2f& pos, const SkColor4f& c, const DrawData& drawData, float radiusMultiplier = 1.0f);
        std::pair<SkPaint, SkPaint> select_tool_line_paint(const DrawData& drawData);
        bool is_actual_selection_tool(DrawingProgramToolType typeToCheck);
        bool is_selection_allowing_tool(DrawingProgramToolType typeToCheck);

        DrawingProgramSelection selection;

        std::unique_ptr<DrawingProgramToolBase> toolToSwitchToAfterUpdate;
        std::unordered_set<CanvasComponentContainer::ObjInfo*> updateableComponents;

        void pen_tool_switch_check();
        enum class TemporaryMoveToolSwitch {
            NONE,
            PAN,
            ZOOM
        };
        bool temporaryEraser = false;
        TemporaryMoveToolSwitch tempMoveToolSwitch = TemporaryMoveToolSwitch::NONE;
        DrawingProgramToolType toolTypeAfterTempMove;

        // Set when the artist presses Esc during an AI build to dismiss the modal dim
        // and keep working; reset to false the frame no request is in flight, so each
        // new build starts modal again. See draw_ai_busy_overlay / ai_busy_modal_active.
        bool aiBusyModalDismissed = false;

        struct GlobalControls {
            std::optional<WorldScalar> lockedCameraScale;

            bool leftClickHeld = false;
            bool middleClickHeld = false;

            DrawingProgramLayerManager::LayerSelector layerSelector = DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED;

            int colorEditing = 0;
        } controls;

        struct DroppedDownloadingFile {
            CanvasComponentContainer::ObjInfo* comp;
            Vector2f windowSizeWhenDropped;
            std::shared_ptr<FileDownloader::DownloadData> downData;
        };
        std::vector<DroppedDownloadingFile> droppedDownloadingFiles;


        std::optional<Vector2f> rightClickPopupLocation;

        uint32_t nextID = 0;

        friend class EyeDropperTool;
        friend class RectDrawTool;
        friend class EllipseDrawTool;
        friend class LineDrawTool;
        friend class BrushTool;
        friend class EraserTool;
        friend class RectSelectTool;
        friend class TextBoxTool;
        friend class ScreenshotTool;
        friend class EditTool;
        friend class MotionPathTool;
        friend class ImageEditTool;
        friend class RectDrawEditTool;
        friend class EllipseDrawEditTool;
        friend class TextBoxEditTool;
        friend class DrawingProgramCache;
        friend class DrawingProgramSelection;
        friend class LassoSelectTool;
        friend class GridEditTool;
        friend class GridModifyTool;
        friend class ZoomCanvasTool;
        friend class PanCanvasTool;
        friend class DrawCamera;
        friend class DrawingProgramLayerManager;
        friend class DrawingProgramLayer;
        friend class DrawingProgramLayerFolder;
        friend class ToolConfiguration;
        // PHASE4 §10: flatten needs selection (skip mid-manipulation
        // comps) + droppedDownloadingFiles (skip still-loading images).
        friend void RasterFlatten::flatten_layer(DrawingProgram& drawP);
        // Blend-aware Merge Down: same access needs as flatten (bakes both
        // layers, checks droppedDownloadingFiles for still-loading images).
        friend bool RasterFlatten::merge_down_baked(DrawingProgram& drawP, DrawingProgramLayerListItem& upper, DrawingProgramLayerListItem& lower);
        // Consolidate Vectors: needs selection (skip mid-manipulation strokes) —
        // same access as flatten.
        friend void RasterFlatten::consolidate_vectors(DrawingProgram& drawP);
        // Optimize Vectors: skips selected (mid-manipulation) vector objects.
        friend void RasterFlatten::optimize_vectors(DrawingProgram& drawP);
        // LAYER-RESOLUTION.md: reduce needs selection (skip mid-manipulation
        // comps) — same as flatten.
        friend void RasterResolution::halve_layer(DrawingProgram& drawP);
};
