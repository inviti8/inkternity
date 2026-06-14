#include "ParticleBrushTool.hpp"
#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../World.hpp"
#include "../../InputManager.hpp"
#include "../../CoordSpaceHelper.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "../../CanvasComponents/ParticleCanvasComponent.hpp"
#include <Helpers/Logger.hpp>

#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"

#ifdef HVYM_HAS_TIMELINEFX_LEGACY
#include "../../CanvasComponents/Particles/FxLibraryStore.hpp"
#endif

ParticleBrushTool::ParticleBrushTool(DrawingProgram& initDrawP):
    DrawingProgramToolBase(initDrawP)
{}

DrawingProgramToolType ParticleBrushTool::get_type() {
    return DrawingProgramToolType::PARTICLEBRUSH;
}

void ParticleBrushTool::gui_toolbox(Toolbar& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    gui.new_id("particle brush tool", [&] {
        text_label_centered(gui, "Particle Brush");
        slider_scalar_field(gui, "particle size", "Size", &brushSize, 0.5f, 30.0f);
        slider_scalar_field(gui, "particle rate", "Rate /s", &rate, 1.0f, 30.0f);
    });
}

void ParticleBrushTool::gui_phone_toolbox(PhoneDrawingProgramScreen& t) {}
void ParticleBrushTool::erase_component(CanvasComponentContainer::ObjInfo*) {}
void ParticleBrushTool::right_click_popup_gui(Toolbar& t, Vector2f popupPos) {}
void ParticleBrushTool::draw(SkCanvas*, const DrawData&) {}
void ParticleBrushTool::switch_tool(DrawingProgramToolType) { painting = false; }
bool ParticleBrushTool::prevent_undo_or_redo() { return false; }

void ParticleBrushTool::tool_update() {
    // Rate-based placement: while the brush is held, deposit effects at `rate`
    // per second at the latest cursor position (hold = pile up; drag = scatter
    // along the stroke).
    if(!painting || rate <= 0.0f) return;
    if(!drawP.layerMan.is_a_layer_being_edited()) { painting = false; return; }
    accum += static_cast<float>(drawP.world.main.deltaTime);
    const float step = 1.0f / rate;
    int n = 0;
    while(accum >= step && n < 10) { stamp(lastCamPos); accum -= step; ++n; }
    if(accum > step) accum = 0.0f;   // drop backlog after a hitch
}

void ParticleBrushTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if(button.button != InputManager::MouseButton::LEFT) return;
    if(button.down) {
        if(!drawP.layerMan.is_a_layer_being_edited()) return;
        if(drawP.world.main.g.gui.cursor_obstructed()) return;
        painting = true;
        accum = 0.0f;
        lastCamPos = button.pos;
        stamp(button.pos);             // one immediately on press
    } else {
        painting = false;
    }
}

void ParticleBrushTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(painting) lastCamPos = motion.pos;
}

void ParticleBrushTool::stamp(Vector2f camPos) {
#ifdef HVYM_HAS_TIMELINEFX_LEGACY
    // Host-authored, like the rest of the particle feature.
    if(drawP.world.netObjMan.is_connected() && !drawP.world.netObjMan.is_server()) {
        Logger::get().log("WORLDFATAL", "[Particle] only the host can place effects");
        return;
    }
    FxLibraryStore* store = drawP.fx_store();
    if(!store || !store->hasActive()) {
        Logger::get().log("INFO", "[Particle] select an effect in the FX Library panel first");
        return;
    }
    const FxLibraryStore::Entry& entry = store->entries()[store->activeLibrary()];

    CanvasComponentContainer* c = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::PARTICLE);
    ParticleCanvasComponent& pc = static_cast<ParticleCanvasComponent&>(c->get_comp());
    pc.d.libraryResourceId = entry.resourceId;
    pc.d.effectName = store->activeEffect();
    pc.d.localScale = brushSize;

    // Place the component origin at the cursor's world position.
    auto& camc = drawP.world.drawData.cam.c;
    c->coords = CoordSpaceHelper(camc.from_space(camPos), camc.inverseScale, camc.rotation);

    auto info = drawP.layerMan.add_component_to_layer_being_edited(c);
    drawP.layerMan.add_undo_place_component(info);
    Logger::get().log("INFO", "[Particle] placed " + store->activeEffect());
#else
    (void)camPos;
#endif
}
