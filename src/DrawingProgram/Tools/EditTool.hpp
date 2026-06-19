#pragma once
#include "DrawingProgramToolBase.hpp"
#include <Helpers/SCollision.hpp>
#include <any>
#include "EditTools/DrawingProgramEditToolBase.hpp"

class DrawingProgram;

class EditTool : public DrawingProgramToolBase {
    public:
        struct HandleData {
            Vector2f* p;
            Vector2f* min;
            Vector2f* max;
            float minimumDistanceBetweenMinAndPoint = MINIMUM_DISTANCE_BETWEEN_BOUNDS;
            float minimumDistanceBetweenMaxAndPoint = MINIMUM_DISTANCE_BETWEEN_BOUNDS;
            Affine2f coordMatrix = Affine2f::Identity();
        };

        EditTool(DrawingProgram& initDrawP);
        virtual DrawingProgramToolType get_type() override;
        virtual void gui_toolbox(Toolbar& t) override;
        virtual void gui_phone_toolbox(PhoneDrawingProgramScreen& t) override;
        virtual void right_click_popup_gui(Toolbar& t, Vector2f popupPos) override;
        virtual void tool_update() override;
        virtual void erase_component(CanvasComponentContainer::ObjInfo* erasedComp) override;
        virtual void switch_tool(DrawingProgramToolType newTool) override;
        virtual void draw(SkCanvas* canvas, const DrawData& drawData) override;
        virtual bool prevent_undo_or_redo() override;
        virtual void input_paste_callback(const CustomEvents::PasteEvent& paste) override;
        virtual void input_text_key_callback(const InputManager::KeyCallbackArgs& key) override;
        virtual void input_text_callback(const InputManager::TextCallbackArgs& text) override;
        virtual void input_key_callback(const InputManager::KeyCallbackArgs& key) override;
        virtual void input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) override;
        virtual void input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) override;
        virtual std::optional<InputManager::TextBoxStartInfo> get_text_box_start_info() override;
        ~EditTool();

        void add_point_handle(const HandleData& handle);
        void edit_start(CanvasComponentContainer::ObjInfo* comp, bool initUndoAfterEditDone = true);
        bool is_editable(CanvasComponentContainer::ObjInfo* comp);

        // PHASE6: re-register handles after the editable point list changed size
        // (clears handles + selection, asks the component edit tool to re-add).
        void refresh_point_handles();
        void toggle_handle_selection(size_t handleIndex);

        std::unique_ptr<DrawingProgramEditToolBase> compEditTool;
        std::vector<HandleData> pointHandles;
        CanvasComponentContainer::ObjInfo* objInfoBeingEdited = nullptr;
        HandleData* pointDragging = nullptr;
        std::any prevData;
        bool undoAfterEditDone;

        // PHASE6 vertex selection: a click (press+release without dragging) on a
        // handle toggles its selection; a drag moves it. selectedHandles holds
        // indices into pointHandles.
        std::vector<size_t> selectedHandles;
        size_t pressedHandleIndex = 0;
        Vector2f pointDownScreenPos{0.0f, 0.0f};
        bool pointDragMoved = false;

        // PHASE6: dragging on empty space (while editing) box-selects vertices —
        // easier than tapping each handle, especially with a stylus. A click (no
        // drag) on empty space still exits edit. Screen-space.
        bool marqueeActive = false;
        Vector2f marqueeStartScreen{0.0f, 0.0f};
        Vector2f marqueeCurScreen{0.0f, 0.0f};
        void select_handles_in_screen_rect(const Vector2f& a, const Vector2f& b);

        std::unique_ptr<CanvasComponent> oldData;
};
