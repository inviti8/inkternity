#include "EraserTool.hpp"
#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../DrawData.hpp"
#include "../../CanvasComponents/MyPaintLayerCanvasComponent.hpp"
#include <include/core/SkBlendMode.h>
#include <include/core/SkPathBuilder.h>

#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"

EraserTool::EraserTool(DrawingProgram& initDrawP):
    DrawingProgramToolBase(initDrawP)
{
    // Bound per-segment eraser cost up-front. Every prior brush stroke's
    // end_stroke pulls its component into unsortedComponents via
    // commit_update → preupdate_component. That bucket only drains on
    // BVH rebuild (default threshold: 1000 components, which is rarely
    // hit in a normal session). Each eraser motion event runs
    // traverse_bvh_run_function, which begins with f(nullptr) — a linear
    // scan of unsortedComponents per segment, per touched-component
    // predicate. After ~100 strokes the scan dominates eraser cost.
    //
    // Rebuilding here moves everything back into the BVH cleanly, so
    // the upcoming eraser-stroke's segment work stays bounded to
    // BVH traversal (log N) instead of scanning the entire unsorted
    // bucket. Cost: one-time ~O(total component count) at tool entry;
    // pays itself off after ~1 second of eraser motion in any non-trivial
    // canvas. Skipped when nothing's unsorted to avoid pointless work.
    if (drawP.drawCache.unsorted_component_count() > 0)
        drawP.drawCache.build({});
}

DrawingProgramToolType EraserTool::get_type() {
    return DrawingProgramToolType::ERASER;
}

void EraserTool::gui_toolbox(Toolbar& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;
    gui.new_id("eraser tool", [&] {
        text_label_centered(gui, "Eraser");
        drawP.world.main.toolConfig.relative_width_gui(drawP, "Size");
    });
}

void EraserTool::gui_phone_toolbox(PhoneDrawingProgramScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    gui.new_id("eraser tool", [&] {
        drawP.world.main.toolConfig.relative_width_gui(drawP, "Size");
    });
}

void EraserTool::right_click_popup_gui(Toolbar& t, Vector2f popupPos) {
    t.paint_popup(popupPos);
}

void EraserTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if(button.button == InputManager::MouseButton::LEFT) {
        if(button.down && !isErasing && !drawP.world.main.g.gui.cursor_obstructed()) {
            isErasing = true;
            erase_between_points(button.pos, button.pos);
        }
        else if(!button.down && isErasing)
            switch_tool(get_type());
    }
}

void EraserTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(isErasing)
        erase_between_points(motion.pos, motion.pos - motion.move);
}

void EraserTool::erase_between_points(const Vector2f& start, const Vector2f& end) {
    auto relativeWidthResult = drawP.world.main.toolConfig.get_relative_width_stroke_size(drawP, drawP.world.drawData.cam.c.inverseScale);
    if(!relativeWidthResult.first.has_value()) {
        drawP.world.main.toolConfig.print_relative_width_fail_message(relativeWidthResult.second);
        isErasing = false;
        return;
    }
    float width = relativeWidthResult.first.value() * 0.5f;

    SCollision::ColliderCollection<float> cC;
    SCollision::generate_wide_line(cC, start, end, width * 2.0f, true);
    auto cCWorld = drawP.world.drawData.cam.c.collider_to_world<SCollision::ColliderCollection<WorldScalar>, SCollision::ColliderCollection<float>>(cC);

    // Single up-front cache invalidation covering the entire segment
    // area. Raster (MyPaintLayer) erases only modify pixels inside this
    // AABB, so per-touched-component invalidates were doing redundant
    // work proportional to (touched components × cached nodes). One
    // call here costs the same regardless of how many strokes overlap
    // the segment. Vector erases still need a per-component invalidate
    // below (the deleted component may extend past the segment, and
    // the cache outside the segment also needs refresh).
    drawP.drawCache.invalidate_cache_at_aabb(cCWorld.bounds);

    // Dispatch helper: vector components are deleted whole (existing
    // behavior); MyPaintLayer components get a destination-out dab pass on
    // their raster surface (PHASE1.md §4 "eraser interaction"). Returns
    // true when the component should be removed from the BVH (vector
    // delete), false when we kept it but mutated its tiles (raster erase).
    auto try_punch_or_mark = [&](auto c) -> bool {
        auto& container = *c->obj;
        // PHASE2 polish: waypoint markers are protected from the eraser.
        // Inkternity's drawing flow has artists drawing alongside placed
        // waypoints; an accidental brush over a marker would lose the
        // waypoint (and any edges into it via the linked-deletion sweep
        // in the layer's eraseCallback). The Edit tool / waypoint tool
        // remain the explicit way to delete a waypoint.
        if (container.get_comp().get_type() == CanvasComponentType::WAYPOINT)
            return false;
        // PHASE8: mask shapes are protected from the eraser. The eraser is meant
        // to be used *inside* a masked area, so consuming the mask itself would
        // be surprising. Delete a mask via the Edit/Select tools instead.
        if (container.get_comp().is_mask())
            return false;
#ifdef HVYM_HAS_LIBMYPAINT
        if (container.get_comp().get_type() == CanvasComponentType::MYPAINTLAYER) {
            auto& layer = static_cast<MyPaintLayerCanvasComponent&>(container.get_comp());
            const Vector2f localStart = container.coords.from_cam_space_to_this(drawP.world, start);
            const Vector2f localEnd   = container.coords.from_cam_space_to_this(drawP.world, end);
            // Camera-space radius -> component-local pixels via two-point
            // distance, robust to rotation/scale between cam and component.
            const Vector2f probe = container.coords.from_cam_space_to_this(drawP.world, start + Vector2f(width, 0));
            const float localRadius = (probe - localStart).norm();
            layer.erase_along_segment(localStart, localEnd, localRadius);
            // No commit_update / no per-component invalidate. The empty-tile
            // skip in erase_along_segment keeps bounds stable across a
            // pure-erase stroke, so calculate_world_bounds (which iterates
            // every allocated tile in the hash map) would be redundant
            // work. mark_dirty inside erase_along_segment already cleared
            // the per-component SkImage cache + bounds cache; the upfront
            // invalidate_cache_at_aabb above covers the node-cache. Wire
            // broadcast still fires; no-op offline.
            container.send_comp_update(drawP, false);
            return false;
        }
#endif
        erasedComponents.emplace(c);
        drawP.drawCache.invalidate_cache_at_optional_aabb(container.get_world_bounds());
        return true;
    };

    // Layer selector dropped per zynx — "Erase from all visible layers"
    // wasn't desired behavior. Eraser always operates on the layer being
    // edited. The shared LayerSelector type is kept for the select tools.
    constexpr auto kEditedLayer = DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED;

    drawP.drawCache.traverse_bvh_run_function(cCWorld.bounds, [&](const auto& bvhNode) {
        if(bvhNode &&
           SCollision::collide(cC, drawP.world.drawData.cam.c.to_space(bvhNode->bounds.min)) &&
           SCollision::collide(cC, drawP.world.drawData.cam.c.to_space(bvhNode->bounds.max)) &&
           SCollision::collide(cC, drawP.world.drawData.cam.c.to_space(bvhNode->bounds.top_right())) &&
           SCollision::collide(cC, drawP.world.drawData.cam.c.to_space(bvhNode->bounds.bottom_left()))) {
            drawP.drawCache.invalidate_cache_at_aabb(bvhNode->bounds);
            drawP.drawCache.traverse_bvh_run_function_starting_at_node_no_collision_check(bvhNode, [&](const auto& bvhNodeChild) {
                drawP.drawCache.node_loop_erase_if_components(bvhNodeChild, [&](auto c) {
                    if(drawP.layerMan.component_passes_layer_selector(c, kEditedLayer))
                        return try_punch_or_mark(c);
                    return false;
                });
                return true;
            });
            return false;
        }
        drawP.drawCache.node_loop_erase_if_components(bvhNode, [&](auto c) {
            if(drawP.layerMan.component_passes_layer_selector(c, kEditedLayer) && c->obj->collides_with(drawP.world.drawData.cam.c, cCWorld, cC))
                return try_punch_or_mark(c);
            return false;
        });
        return true;
    });
}

void EraserTool::erase_component(CanvasComponentContainer::ObjInfo* erasedComp) {
    erasedComponents.erase(erasedComp);
}

void EraserTool::switch_tool(DrawingProgramToolType newTool) {
    drawP.layerMan.erase_component_container(erasedComponents);
    isErasing = false;
}

void EraserTool::tool_update() {
    if(!drawP.world.main.g.gui.cursor_obstructed())
        drawP.world.main.input.hideCursor = true;
}

bool EraserTool::prevent_undo_or_redo() {
    return drawP.controls.leftClickHeld;
}

void EraserTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    if(!drawP.world.main.input.isTouchDevice && !drawData.main->g.gui.cursor_obstructed()) {
        if(!lastPosOpt.has_value())
            lastPosOpt = drawData.main->input.mouse.pos;
        auto& lastPos = lastPosOpt.value();

        auto relativeWidthResult = drawP.world.main.toolConfig.get_relative_width_stroke_size(drawP, drawP.world.drawData.cam.c.inverseScale);
        if(relativeWidthResult.first.has_value()) {
            float width = relativeWidthResult.first.value() * 0.5f;
            SkPaint linePaint;
            linePaint.setAntiAlias(drawData.skiaAA);
            linePaint.setColor4f({0.0f, 0.0f, 0.0f, 0.4f});
            linePaint.setStyle(SkPaint::kStroke_Style);
            linePaint.setStrokeCap(SkPaint::kRound_Cap);
            linePaint.setStrokeWidth(width * 2.0f + 1.0f);
            SkPathBuilder erasePath;
            erasePath.moveTo(convert_vec2<SkPoint>(lastPos));
            erasePath.lineTo(convert_vec2<SkPoint>(drawData.main->input.mouse.pos));

            SkPath erasePathDetach = erasePath.detach();
            canvas->drawPath(erasePathDetach, linePaint);

            linePaint.setStrokeWidth(width * 2.0f);
            linePaint.setColor4f({1.0f, 1.0f, 1.0f, 0.4f});
            canvas->drawPath(erasePathDetach, linePaint);
        }

        lastPos = drawData.main->input.mouse.pos;
    }
    else
        lastPosOpt = std::nullopt;
}
