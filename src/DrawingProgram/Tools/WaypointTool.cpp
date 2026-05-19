#include "WaypointTool.hpp"

#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "../../World.hpp"
#include "../../ResourceManager.hpp"
#include "../../CanvasComponents/ImageCanvasComponent.hpp"
#include "../../CanvasComponents/WaypointCanvasComponent.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "../../DrawingProgram/Layers/DrawingProgramLayerManager.hpp"
#include "../../DrawingProgram/Layers/DrawingProgramLayerListItem.hpp"
#include "../../Waypoints/Waypoint.hpp"
#include "../../Waypoints/WaypointGraph.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/CheckBoxHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "../../GUIStuff/Elements/DropDown.hpp"
#include "Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp"
#include "Helpers/SCollision.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkPngEncoder.h>

#include <SDL3/SDL_keyboard.h>

WaypointTool::WaypointTool(DrawingProgram& initDrawP)
    : DrawingProgramToolBase(initDrawP) {}

DrawingProgramToolType WaypointTool::get_type() {
    return DrawingProgramToolType::WAYPOINT;
}

void WaypointTool::switch_tool(DrawingProgramToolType) {
    drawP.world.wpGraph.clear_selection();
}

void WaypointTool::erase_component(CanvasComponentContainer::ObjInfo*) {
    // The selected waypoint's marker getting erased doesn't necessarily
    // erase the waypoint itself (the canvas component is just a visual
    // proxy), so don't drop selection here. M5 doesn't actually wire
    // erase to delete the Waypoint anyway — that's a follow-up.
}

void WaypointTool::tool_update() {}

void WaypointTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    // Anchor for an edge endpoint or a framing-rect overlay: the screen
    // position of the waypoint's framing-rect center in current cam-space.
    const auto wp_anchor_in_cam_space = [&](const Waypoint& wp) -> Vector2f {
        const Vector<int32_t, 2> ws = wp.get_window_size();
        const Vector2f localCenter(static_cast<float>(ws.x()) * 0.5f,
                                   static_cast<float>(ws.y()) * 0.5f);
        return wp.get_coords().from_this_to_cam_space(drawP.world, localCenter);
    };

    // Faint outgoing-edge previews — PHASE1.md §5 author-mode chrome.
    // Drawn for every edge in the graph, not just those touching the
    // current selection: the visual is "the directed graph as-is".
    auto& edges = drawP.world.wpGraph.get_edges();
    if (edges) {
        SkPaint edgePaint;
        edgePaint.setAntiAlias(drawData.skiaAA);
        edgePaint.setStyle(SkPaint::kStroke_Style);
        edgePaint.setStrokeWidth(0.0f);
        edgePaint.setColor4f({0.88f, 0.69f, 0.25f, 0.45f});  // muted gold, semi-transparent
        for (auto& info : *edges) {
            auto fromRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(info.obj->get_from());
            auto toRef   = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(info.obj->get_to());
            if (!fromRef || !toRef) continue;
            const Vector2f a = wp_anchor_in_cam_space(*fromRef);
            const Vector2f b = wp_anchor_in_cam_space(*toRef);
            canvas->drawLine(a.x(), a.y(), b.x(), b.y(), edgePaint);
        }
    }

    // Selected waypoint's framing-rect outline.
    if (!drawP.world.wpGraph.has_selection()) return;
    auto wpRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(drawP.world.wpGraph.get_selected());
    if (!wpRef) return;
    const auto& coords = wpRef->get_coords();
    const Vector<int32_t, 2> ws = wpRef->get_window_size();
    if (ws.x() <= 0 || ws.y() <= 0) return;

    const Vector2f corners[4] = {
        coords.from_this_to_cam_space(drawP.world, Vector2f(0.0f, 0.0f)),
        coords.from_this_to_cam_space(drawP.world, Vector2f(static_cast<float>(ws.x()), 0.0f)),
        coords.from_this_to_cam_space(drawP.world, Vector2f(static_cast<float>(ws.x()), static_cast<float>(ws.y()))),
        coords.from_this_to_cam_space(drawP.world, Vector2f(0.0f, static_cast<float>(ws.y()))),
    };

    SkPathBuilder pb;
    pb.moveTo(corners[0].x(), corners[0].y());
    for (int i = 1; i < 4; ++i) pb.lineTo(corners[i].x(), corners[i].y());
    pb.close();

    SkPaint outline;
    outline.setAntiAlias(drawData.skiaAA);
    outline.setStyle(SkPaint::kStroke_Style);
    outline.setStrokeWidth(0.0f);
    outline.setColor4f({0.88f, 0.69f, 0.25f, 1.0f});  // matches the marker fill — same gold
    canvas->drawPath(pb.detach(), outline);
}

// FRAME_ANIM.md §3 — settings-panel block for the Frame step axis radio.
// Shared between the desktop and phone tool panels.
namespace {
void gui_frame_step_block(GUIStuff::GUIManager& gui,
                          FrameStepAxis& axisChoice) {
    using namespace GUIStuff::ElementHelpers;
    text_label(gui, "Frame step");
    radio_button_selector<FrameStepAxis>(gui, "frame step axis", &axisChoice, {
        {"+X", FrameStepAxis::POS_X},
        {"-X", FrameStepAxis::NEG_X},
        {"+Y", FrameStepAxis::POS_Y},
        {"-Y", FrameStepAxis::NEG_Y}
    });
}
}  // namespace

void WaypointTool::gui_toolbox(Toolbar&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    gui.new_id("waypoint tool", [&] {
        text_label_centered(gui, "Waypoint");
        bool renderedSelectedBlock = false;
        if (drawP.world.wpGraph.has_selection()) {
            auto wpRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(drawP.world.wpGraph.get_selected());
            if (wpRef) {
                renderedSelectedBlock = true;
                input_text_field(gui, "label", "Label", &wpRef->mutable_label(), {
                    // P0.5-LIVE-SYNC: publish label changes so already-
                    // connected subscribers see the rename immediately
                    // (instead of having to disconnect/reconnect for the
                    // initial-state snapshot to pick up the new value).
                    .onEdit = [wpRef] { Waypoint::publish_label_update(wpRef); }
                });
                // PHASE2 M4: per-waypoint reader-mode transition speed.
                // Range 0.1× (slow) to 2× (fast); default 1× = global speed.
                slider_scalar_field<float>(gui, "transition speed", "Transition speed",
                    &wpRef->mutable_transition_speed_multiplier(),
                    Waypoint::TRANSITION_SPEED_MIN, Waypoint::TRANSITION_SPEED_MAX,
                    { .decimalPrecision = 2,
                      .onEdit = [wpRef] { Waypoint::publish_transition_speed_update(wpRef); } });
                // PHASE2 M5: easing dropdown. The DropDown<T> widget reads
                // *(size_t*)d as the selection index, so we cast through
                // a uint8 alias of the enum (TransitionEasing values
                // 0..4 correspond to dropdown options in order).
                left_to_right_line_layout(gui, [&]() {
                    text_label(gui, "Easing");
                    gui.element<DropDown<uint8_t>>("easing dropdown",
                        reinterpret_cast<uint8_t*>(&wpRef->mutable_transition_easing()),
                        transition_easing_display_names(),
                        DropdownOptions{
                            .onClick = [wpRef] { Waypoint::publish_transition_easing_update(wpRef); }
                        });
                });
                // TRANSITIONS.md — transition-point flag + stop-time
                // slider (slider only renders when the flag is on).
                checkbox_boolean_field(gui, "is transition", "Transition point",
                    &wpRef->mutable_is_transition(),
                    [this, wpRef] {
                        invalidate_marker_caches();
                        Waypoint::publish_is_transition_update(wpRef);
                    });
                if (wpRef->is_transition()) {
                    slider_scalar_field<float>(gui, "stop time", "Stop time (s)",
                        &wpRef->mutable_stop_time(),
                        Waypoint::TRANSITION_STOP_TIME_MIN, Waypoint::TRANSITION_STOP_TIME_MAX,
                        { .decimalPrecision = 1,
                          .onEdit = [wpRef] { Waypoint::publish_stop_time_update(wpRef); } });
                    // T6: invariant-violation prompt. Shown whenever a
                    // transition-flagged waypoint has 2+ outgoing edges
                    // (most commonly right after the user just toggled
                    // the flag on). Stateless — re-evaluates each frame
                    // from the live graph state, so navigating away and
                    // back leaves the prompt in place until acted on.
                    const auto selId = drawP.world.wpGraph.get_selected();
                    const size_t outCount = drawP.world.wpGraph.count_outgoing_edges_from(selId);
                    if (outCount >= 2) {
                        text_label(gui, "Has " + std::to_string(outCount) + " outgoing edges; transitions allow only 1.");
                        text_button(gui, "prune outgoing", "Keep first edge",
                            { .onClick = [this, selId] {
                                drawP.world.wpGraph.prune_outgoing_edges_to_first(selId);
                            }});
                        text_button(gui, "cancel transition", "Cancel transition",
                            { .onClick = [this, selId] {
                                auto ref = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(selId);
                                if (ref) ref->set_is_transition(false);
                                invalidate_marker_caches();
                            }});
                    }
                }
            }
        }
        if (!renderedSelectedBlock)
            text_label(gui, "Click an existing marker, or click empty canvas to drop one.");

        // FRAME_ANIM.md §3 — Next Frame + axis radio + onion-skin toggle.
        // Always rendered; Next Frame works without a selection.
        gui_frame_step_block(gui, frameStepAxis);
        text_button(gui, "next frame", "Next frame",
            { .onClick = [this] { next_frame_step(frameStepAxis); } });
        slider_scalar_field<int>(gui, "copy layer offset", "Copy layer offset",
            &frameCopyLayerOffset,
            FRAME_COPY_LAYER_OFFSET_MIN, FRAME_COPY_LAYER_OFFSET_MAX);
        text_button(gui, "copy frame", "Copy frame from selection",
            { .onClick = [this] { copy_frame_to_current(); } });
    });
}

void WaypointTool::gui_phone_toolbox(PhoneDrawingProgramScreen&) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& gui = drawP.world.main.g.gui;
    gui.new_id("waypoint tool phone", [&] {
        if (drawP.world.wpGraph.has_selection()) {
            auto wpRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(drawP.world.wpGraph.get_selected());
            if (wpRef) {
                input_text_field(gui, "label", "Label", &wpRef->mutable_label(), {
                    // P0.5-LIVE-SYNC — see desktop variant for notes.
                    .onEdit = [wpRef] { Waypoint::publish_label_update(wpRef); }
                });
                slider_scalar_field<float>(gui, "transition speed", "Transition speed",
                    &wpRef->mutable_transition_speed_multiplier(),
                    Waypoint::TRANSITION_SPEED_MIN, Waypoint::TRANSITION_SPEED_MAX,
                    { .decimalPrecision = 2,
                      .onEdit = [wpRef] { Waypoint::publish_transition_speed_update(wpRef); } });
                // PHASE2 M5: easing dropdown. The DropDown<T> widget reads
                // *(size_t*)d as the selection index, so we cast through
                // a uint8 alias of the enum (TransitionEasing values
                // 0..4 correspond to dropdown options in order).
                left_to_right_line_layout(gui, [&]() {
                    text_label(gui, "Easing");
                    gui.element<DropDown<uint8_t>>("easing dropdown",
                        reinterpret_cast<uint8_t*>(&wpRef->mutable_transition_easing()),
                        transition_easing_display_names(),
                        DropdownOptions{
                            .onClick = [wpRef] { Waypoint::publish_transition_easing_update(wpRef); }
                        });
                });
                checkbox_boolean_field(gui, "is transition", "Transition point",
                    &wpRef->mutable_is_transition(),
                    [this, wpRef] {
                        invalidate_marker_caches();
                        Waypoint::publish_is_transition_update(wpRef);
                    });
                if (wpRef->is_transition()) {
                    slider_scalar_field<float>(gui, "stop time", "Stop time (s)",
                        &wpRef->mutable_stop_time(),
                        Waypoint::TRANSITION_STOP_TIME_MIN, Waypoint::TRANSITION_STOP_TIME_MAX,
                        { .decimalPrecision = 1,
                          .onEdit = [wpRef] { Waypoint::publish_stop_time_update(wpRef); } });
                    // T6 — see desktop variant for full notes; identical logic.
                    const auto selId = drawP.world.wpGraph.get_selected();
                    const size_t outCount = drawP.world.wpGraph.count_outgoing_edges_from(selId);
                    if (outCount >= 2) {
                        text_label(gui, "Has " + std::to_string(outCount) + " outgoing edges; transitions allow only 1.");
                        text_button(gui, "prune outgoing", "Keep first edge",
                            { .onClick = [this, selId] {
                                drawP.world.wpGraph.prune_outgoing_edges_to_first(selId);
                            }});
                        text_button(gui, "cancel transition", "Cancel transition",
                            { .onClick = [this, selId] {
                                auto ref = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(selId);
                                if (ref) ref->set_is_transition(false);
                                invalidate_marker_caches();
                            }});
                    }
                }
            }
        }
        // FRAME_ANIM.md §3 — same Next Frame + axis radio + onion-skin
        // controls as the desktop panel. Always rendered.
        gui_frame_step_block(gui, frameStepAxis);
        text_button(gui, "next frame", "Next frame",
            { .onClick = [this] { next_frame_step(frameStepAxis); } });
        slider_scalar_field<int>(gui, "copy layer offset", "Copy layer offset",
            &frameCopyLayerOffset,
            FRAME_COPY_LAYER_OFFSET_MIN, FRAME_COPY_LAYER_OFFSET_MAX);
        text_button(gui, "copy frame", "Copy frame from selection",
            { .onClick = [this] { copy_frame_to_current(); } });
    });
}

void WaypointTool::invalidate_marker_caches() {
    drawP.drawCache.clear_own_cached_surfaces();
}

void WaypointTool::next_frame_step(FrameStepAxis axis) {
    // FRAME_ANIM.md §3 — snap the editor camera by exactly one window
    // viewport in the chosen axis. The offset is taken in the live
    // camera's own local (screen) space, then projected to world via
    // (from_space(offset) − from_space(0)). This handles rotation
    // correctly: stepping +X always moves the view to *screen-right*,
    // regardless of how the camera is rotated relative to the world.
    const Vector2f windowSize = drawP.world.main.window.size.cast<float>();
    Vector2f screenOffset{0.0f, 0.0f};
    switch (axis) {
        case FrameStepAxis::POS_X: screenOffset = { windowSize.x(), 0.0f}; break;
        case FrameStepAxis::NEG_X: screenOffset = {-windowSize.x(), 0.0f}; break;
        case FrameStepAxis::POS_Y: screenOffset = {0.0f,  windowSize.y()}; break;
        case FrameStepAxis::NEG_Y: screenOffset = {0.0f, -windowSize.y()}; break;
    }

    const CoordSpaceHelper& current = drawP.world.drawData.cam.c;
    const WorldVec worldOffset = current.from_space(screenOffset) - current.from_space({0.0f, 0.0f});

    CoordSpaceHelper target = current;
    target.pos = current.pos + worldOffset;
    drawP.world.drawData.cam.smooth_move_to(drawP.world, target, windowSize);

    // Stash the just-cleared selection as the pending chain source.
    // When the artist drops a new waypoint after Next Frame,
    // drop_waypoint sees this and auto-wires an edge from the previous
    // waypoint to the new one — so the chain builds without the artist
    // having to shift+click on an offscreen previous waypoint. Clearing
    // wpGraph selection lets the settings panel render its
    // "no waypoint selected" state cleanly (no stale label-input
    // caching to fight).
    if (drawP.world.wpGraph.has_selection()) {
        pendingChainSourceId = drawP.world.wpGraph.get_selected();
        hasPendingChainSource = true;
        drawP.world.wpGraph.clear_selection();
    }
}

void WaypointTool::copy_frame_to_current() {
    // FRAME_ANIM.md §4 — one-shot raster paste of the PREVIOUS frame's
    // active-layer content into the live viewport. "Previous" is
    // resolved via the waypoint graph: the from-node of the FIRST
    // incoming edge to the currently-selected waypoint. The first-edge
    // tiebreaker matches the TRANSITIONS.md §5 convention.
    if (!drawP.layerMan.is_a_layer_being_edited()) return;
    if (!drawP.world.wpGraph.has_selection()) return;
    const NetworkingObjects::NetObjID currentId = drawP.world.wpGraph.get_selected();

    // Walk edges for the first one whose .to == current; its .from is
    // the source. No incoming edge → no previous frame → no-op.
    NetworkingObjects::NetObjID sourceId{};
    bool foundIncoming = false;
    auto& edges = drawP.world.wpGraph.get_edges();
    if (edges) {
        for (auto& info : *edges) {
            if (info.obj->get_to() == currentId) {
                sourceId = info.obj->get_from();
                foundIncoming = true;
                break;
            }
        }
    }
    if (!foundIncoming) return;

    auto srcRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(sourceId);
    if (!srcRef) return;

    const Vector<int32_t, 2> sourceWindow = srcRef->get_window_size();
    if (sourceWindow.x() <= 0 || sourceWindow.y() <= 0) return;  // pre-Phase-2 save artefact guard

    auto editingLayerPtr = drawP.layerMan.get_editing_layer().lock();
    if (!editingLayerPtr) return;

    // 1. Render ONLY the currently-edited layer's content at the source
    // frame's view into an offscreen surface sized to that frame's
    // windowSize. Skips the live draw-cache (takingScreenshot=true) and
    // keeps the offscreen transparent where the layer has nothing.
    SkImageInfo info = SkImageInfo::Make(sourceWindow.x(), sourceWindow.y(),
                                         kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return;
    SkCanvas* off = surface->getCanvas();
    off->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});

    DrawData oc = drawP.world.drawData;
    oc.cam.c = srcRef->get_coords();
    oc.cam.set_viewing_area(sourceWindow.cast<float>());
    oc.takingScreenshot       = true;
    oc.transparentBackground  = true;
    oc.drawGrids              = false;
    oc.refresh_draw_optimizing_values();
    editingLayerPtr->draw(off, oc);

    // 2. Encode the offscreen as PNG bytes so it can ride the existing
    // ResourceData / ResourceManager path (same one used for dropped
    // image files and clipboard pastes).
    sk_sp<SkImage> snapshot = surface->makeImageSnapshot();
    if (!snapshot) return;
    sk_sp<SkImage> raster = snapshot->makeRasterImage(nullptr);
    if (!raster) raster = snapshot;
    SkPixmap pix;
    if (!raster->peekPixels(&pix)) return;
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pix, {})) return;
    sk_sp<SkData> pngData = stream.detachAsData();
    if (!pngData) return;

    // 3. Register the PNG as a resource and grab its NetObjID.
    ResourceData newResource;
    newResource.data = std::make_shared<std::string>(
        reinterpret_cast<const char*>(pngData->bytes()), pngData->size());
    newResource.name = "frame_copy.png";
    NetworkingObjects::NetObjID imageID =
        drawP.world.rMan.add_resource(newResource).get_net_id();

    // 4. Resolve destination layer. Walk the flattened layer order; if
    // current_index + offset is out of bounds, fall back to the
    // currently-edited layer (per the design's "defaults to same layer"
    // contract).
    DrawingProgramLayerListItem* destLayer = editingLayerPtr.get();
    if (frameCopyLayerOffset != 0) {
        auto flat = drawP.layerMan.get_flattened_layer_list();
        int curIdx = -1;
        for (size_t i = 0; i < flat.size(); ++i) {
            if (flat[i] == editingLayerPtr.get()) {
                curIdx = static_cast<int>(i);
                break;
            }
        }
        if (curIdx >= 0) {
            const int targetIdx = curIdx + frameCopyLayerOffset;
            if (targetIdx >= 0 && targetIdx < static_cast<int>(flat.size()))
                destLayer = flat[targetIdx];
        }
    }

    // 5. Build the ImageCanvasComponent and place it on destLayer at
    // the live camera's framing rect. coords = the live cam-space
    // (matches drop_waypoint pattern), p1/p2 = the live window's pixel
    // extent in that local space. The image lands exactly where the
    // artist's current view is, in world space.
    CanvasComponentContainer* container = new CanvasComponentContainer(
        drawP.world.netObjMan, CanvasComponentType::IMAGE);
    container->coords = drawP.world.drawData.cam.c;
    auto& img = static_cast<ImageCanvasComponent&>(container->get_comp());
    img.d.p1 = Vector2f{0.0f, 0.0f};
    img.d.p2 = drawP.world.main.window.size.cast<float>();
    img.d.imageID = imageID;

    // Insert into the destination layer's component list (same call
    // shape as add_component_to_layer_being_edited, just on an explicit
    // layer instead of editingLayer). commit_update + send_comp_update
    // finalise world bounds and broadcast to subscribers; the undo
    // entry lets the artist undo the paste if they pasted on the wrong
    // layer.
    auto& destComponents = destLayer->get_layer().components;
    auto newIt = destComponents->push_back_and_send_create(destComponents, container);
    CanvasComponentContainer::ObjInfo* objInfo = &(*newIt);
    container->commit_update(drawP);
    container->send_comp_update(drawP, true);
    if (container->get_world_bounds().has_value())
        drawP.layerMan.add_undo_place_component(objInfo);
}

void WaypointTool::right_click_popup_gui(Toolbar&, Vector2f) {}
bool WaypointTool::prevent_undo_or_redo() { return false; }

void WaypointTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if (button.button != InputManager::MouseButton::LEFT) return;
    if (!button.down) return;
    if (drawP.world.main.g.gui.cursor_obstructed()) return;
    if (!drawP.layerMan.is_a_layer_being_edited()) return;

    // Shift+click on a waypoint creates an edge from the currently selected
    // waypoint to the clicked one. Provides a way to test edges before M6
    // lands the tree-window edge editor; remains a useful shortcut after.
    const bool shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (shiftHeld && drawP.world.wpGraph.has_selection()) {
        if (try_create_edge_to_clicked(button.pos)) return;
    }

    // Hit-test first; clicking on an existing waypoint focuses the camera
    // there. Only fall through to dropping a new waypoint if the click
    // missed every existing marker.
    if (try_focus_existing_waypoint(button.pos)) return;
    drop_waypoint(button.pos);
}

bool WaypointTool::try_focus_existing_waypoint(const Vector2f& clickPos) {
    // Build a small click collider around the cursor — same wide-line
    // pattern EraserTool uses, with start == end so it reduces to a
    // disc. Width is matched to the marker so a click anywhere within
    // the marker's footprint registers.
    SCollision::ColliderCollection<float> cC;
    SCollision::generate_wide_line(cC, clickPos, clickPos,
                                   WaypointCanvasComponent::MARKER_RADIUS_PX * 2.0f, true);
    const auto cCWorld = drawP.world.drawData.cam.c.collider_to_world<SCollision::ColliderCollection<WorldScalar>, SCollision::ColliderCollection<float>>(cC);

    NetworkingObjects::NetObjID hitId{};
    bool hit = false;
    drawP.drawCache.traverse_bvh_run_function(cCWorld.bounds, [&](const auto& bvhNode) {
        drawP.drawCache.node_loop_components(bvhNode, [&](auto c) {
            if (hit) return;
            if (c->obj->get_comp().get_type() != CanvasComponentType::WAYPOINT) return;
            if (!c->obj->collides_with(drawP.world.drawData.cam.c, cCWorld, cC)) return;
            const auto& wpc = static_cast<const WaypointCanvasComponent&>(c->obj->get_comp());
            hitId = wpc.get_waypoint_id();
            hit = true;
        });
        return !hit;
    });
    if (!hit) return false;

    auto wpRef = drawP.world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(hitId);
    if (!wpRef) return false;  // dangling ref — treat as miss
    drawP.world.drawData.cam.smooth_move_to(drawP.world, wpRef->get_coords(), wpRef->get_window_size().cast<float>());
    drawP.world.wpGraph.select(hitId);
    return true;
}

bool WaypointTool::try_create_edge_to_clicked(const Vector2f& clickPos) {
    // Reuse the focus-path's hit-test (single-point wide-line collider)
    // but DON'T focus the camera. Just find the hit waypoint id and add
    // an edge.
    SCollision::ColliderCollection<float> cC;
    SCollision::generate_wide_line(cC, clickPos, clickPos,
                                   WaypointCanvasComponent::MARKER_RADIUS_PX * 2.0f, true);
    const auto cCWorld = drawP.world.drawData.cam.c.collider_to_world<SCollision::ColliderCollection<WorldScalar>, SCollision::ColliderCollection<float>>(cC);

    NetworkingObjects::NetObjID hitId{};
    bool hit = false;
    drawP.drawCache.traverse_bvh_run_function(cCWorld.bounds, [&](const auto& bvhNode) {
        drawP.drawCache.node_loop_components(bvhNode, [&](auto c) {
            if (hit) return;
            if (c->obj->get_comp().get_type() != CanvasComponentType::WAYPOINT) return;
            if (!c->obj->collides_with(drawP.world.drawData.cam.c, cCWorld, cC)) return;
            hitId = static_cast<const WaypointCanvasComponent&>(c->obj->get_comp()).get_waypoint_id();
            hit = true;
        });
        return !hit;
    });
    if (!hit) return false;
    const auto sel = drawP.world.wpGraph.get_selected();
    if (hitId == sel) return false;  // self-edge: skip

    drawP.world.wpGraph.add_edge_enforcing_invariant(sel, hitId, std::optional<std::string>{});
    return true;
}

void WaypointTool::drop_waypoint(const Vector2f& clickPos) {
    using namespace NetworkingObjects;

    // Snapshot the current camera into a Waypoint and drop it into the
    // graph. Label is empty for now — M5-b will add a label-edit affordance.
    const CoordSpaceHelper currentCam = drawP.world.drawData.cam.c;
    const Vector<int32_t, 2> windowSize = drawP.world.main.window.size.cast<int32_t>();
    auto& nodes = drawP.world.wpGraph.get_nodes();
    auto wpIt = nodes->emplace_back_direct(nodes, std::string{}, currentCam, windowSize);
    const NetObjID newWaypointId = wpIt->obj.get_net_id();

    // Container.coords mirrors the camera at click time, so clickPos
    // (cam-space) lands at component-local clickPos as the marker
    // position — same convention as BrushStrokeCanvasComponent's
    // first point.
    auto* container = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::WAYPOINT);
    container->coords = currentCam;
    auto& wpc = static_cast<WaypointCanvasComponent&>(container->get_comp());
    wpc.set_data(newWaypointId, clickPos);

    auto* objInfo = drawP.layerMan.add_component_to_layer_being_edited(container);
    container->commit_update(drawP);
    container->send_comp_update(drawP, true);
    if (container->get_world_bounds().has_value())
        drawP.layerMan.add_undo_place_component(objInfo);

    // FRAME_ANIM.md §4 — if the previous gesture was Next Frame, the
    // selection at that moment was stashed as pendingChainSourceId.
    // Auto-wire an edge `pending -> new` so the animation chain builds
    // without the artist having to shift+click an offscreen previous
    // waypoint. Uses add_edge_enforcing_invariant so the transition-
    // point single-out rule (TRANSITIONS.md §4) is respected if the
    // previous waypoint happens to be a transition.
    if (hasPendingChainSource) {
        drawP.world.wpGraph.add_edge_enforcing_invariant(
            pendingChainSourceId, newWaypointId, std::optional<std::string>{});
        hasPendingChainSource = false;
    }

    drawP.world.wpGraph.select(newWaypointId);
}
