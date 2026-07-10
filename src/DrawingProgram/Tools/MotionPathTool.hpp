#pragma once
#include "DrawingProgramToolBase.hpp"
#include "../Layers/MotionPath.hpp"
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <optional>

class DrawingProgram;
struct DrawData;
class DrawingProgramLayerListItem;

// MOTION-PATH.md P2 — canvas editor for a flip-book group's animation path.
// Editor-only chrome: it's a tool, so it's never active in reader/viewer mode.
// Bound to the folder whose path it edits (resolved by NetObjID each frame, so it
// survives the folder moving in the tree). Draws the bezier curve + green(first)/
// red(last)/yellow(middle) diamond node handles + tangent handles; edits nodes with
// a self-contained interaction (Route 3 — no shared EditTool coupling). The path
// itself is stored on the folder (MotionPath.hpp); this tool only edits it.
class MotionPathTool : public DrawingProgramToolBase {
    public:
        MotionPathTool(DrawingProgram& initDrawP, NetworkingObjects::NetObjID initFolderId = NetworkingObjects::NetObjID{0, 0});

        virtual DrawingProgramToolType get_type() override;
        virtual void gui_toolbox(Toolbar& t) override;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        virtual void tool_update() override;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        virtual void switch_tool(DrawingProgramToolType newTool) override;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) override;
        virtual bool prevent_undo_or_redo() override;
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override;

    private:
        // Resolve the bound folder / its path (null if the folder or path is gone).
        DrawingProgramLayerListItem* folder() const;
        MotionPath* path() const;
        // Push the path's edited state to peers + relayout (whole-struct sync).
        void commit();
        // Shared inspector body (desktop + phone toolboxes both call it).
        void build_toolbox();
        // Undo batching: begin_edit snapshots the path at a session start (no-op
        // if one is pending); end_edit pushes a swap-based undo if it changed and
        // clears the snapshot. Sessions bound drags, discrete ops, and inspector
        // edits (flushed on selection change / tool exit).
        void begin_edit();
        void end_edit();

        NetworkingObjects::NetObjID folderId;

        // Selection + drag state (session-only).
        std::optional<size_t> selectedNode;   // index into points
        // A drag targets either a node (isTangent=false) or one of its tangent
        // handles (isTangent=true, tangentIn picks controlIn vs controlOut).
        bool dragging = false;
        size_t dragNode = 0;
        bool dragTangent = false;
        bool dragTangentIn = false;
        bool dragMoved = false;
        Vector2f dragDownScreen{0.0f, 0.0f};

        // Snapshot of the path at the current edit-session start (for undo).
        std::optional<MotionPath> undoBaseline;
};
