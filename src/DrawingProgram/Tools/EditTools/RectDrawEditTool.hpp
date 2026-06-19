#pragma once
#include "DrawingProgramEditToolBase.hpp"
#include "../../../CanvasComponents/RectangleCanvasComponent.hpp"
#include <Helpers/SCollision.hpp>
#include <any>

class DrawingProgram;

class RectDrawEditTool : public DrawingProgramEditToolBase {
    public:
        RectDrawEditTool(DrawingProgram& initDrawP, CanvasComponentContainer::ObjInfo* initComp);
        virtual void edit_start(EditTool& editTool, std::any& prevData) override;
        virtual void register_handles(EditTool& editTool) override;
        virtual bool wants_handle_refresh_on_drag() const override;   // PHASE8: tangent coordMatrices track nodes
        virtual void commit_edit_updates(std::any& prevData) override;
        virtual bool edit_update() override;
        virtual void edit_gui(Toolbar& t) override;
    private:
        std::optional<RectangleCanvasComponent::Data> oldData;
        EditTool* editTool = nullptr;   // PHASE6: for polygon vertex selection / Add Point
};
