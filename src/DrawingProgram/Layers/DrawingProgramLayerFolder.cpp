#include "DrawingProgramLayerFolder.hpp"
#include "DrawingProgramLayerListItem.hpp"
#include "DrawingProgramLayer.hpp"
#include <algorithm>
#include <Helpers/Parallel.hpp>
#include "DrawingProgramLayerManager.hpp"
#include "../DrawingProgram.hpp"
#include "../../World.hpp"
#include "../../MainProgram.hpp"

void DrawingProgramLayerFolder::draw(SkCanvas* canvas, const DrawData& drawData) const {
    for(auto& p : (*folderList) | std::views::reverse)
        p.obj->draw(canvas, drawData);
}

size_t DrawingProgramLayerFolder::frame_count() const {
    return folderList ? folderList->size() : 0;
}

void DrawingProgramLayerFolder::draw_flipbook_frame(SkCanvas* canvas, const DrawData& drawData) const {
    const size_t n = frame_count();
    if(n == 0)
        return;
    // frameIndex IS the panel/child index: plain iteration of folderList yields
    // index 0 first, and index 0 is the panel's TOP row (it draws last in the
    // normal reverse composite, so it sits on top).
    size_t idx = flipbookRuntime.frameIndex;
    if(idx >= n)
        idx = n - 1;

    // Draw the child at panel index k, optionally at reduced alpha (onion ghost).
    auto draw_child_at = [&](size_t k, float alpha) {
        if(k >= n) return;
        size_t i = 0;
        for(auto& p : *folderList) {
            if(i == k) {
                if(alpha < 1.0f) {
                    SkPaint lp;
                    lp.setAlphaf(alpha);
                    canvas->saveLayer(nullptr, &lp);
                    p.obj->draw(canvas, drawData);
                    canvas->restore();
                }
                else
                    p.obj->draw(canvas, drawData);
                return;
            }
            ++i;
        }
    };

    // Onion skin: while EDITING this flip-book in drawing mode (not playing), a
    // flip-book shows only one frame — so ghost the adjacent frames at half alpha
    // so the artist can register against the frame below/above. Only for the group
    // currently being edited (its editing layer is a direct child), to avoid
    // ghosting every flip-book on the canvas.
    bool showOnion = false;
    if(flipbookRuntime.onionSkin && !flipbookRuntime.previewPlaying && !flipbookRuntime.playing &&
       drawData.main && drawData.main->world && !drawData.main->world->readerMode.is_active()) {
        auto editing = drawData.main->world->drawProg.layerMan.get_editing_layer();
        DrawingProgramLayerListItem* el = editing.expired() ? nullptr : editing.lock().get();
        if(el)
            for(auto& p : *folderList)
                if(&(*p.obj) == el) { showOnion = true; break; }
    }
    if(showOnion) {
        if(idx >= 1)     draw_child_at(idx - 1, 0.5f);   // previous frame
        if(idx + 1 < n)  draw_child_at(idx + 1, 0.5f);   // next frame
    }
    draw_child_at(idx, 1.0f);   // active frame on top
}

// Advance flipbookRuntime.frameIndex by one frame per the play style. Panel
// index space: 0 = top row = frame 1. "forward" (fwd) is top -> bottom by
// default, reversed by invert.
static void flipbook_step_once(DrawingProgramLayerFolder::FlipbookRuntime& rt, size_t n, FlipbookPlayStyle style, bool invert) {
    const int fwd = invert ? -1 : 1;
    long idx = static_cast<long>(rt.frameIndex);
    switch(style) {
        case FlipbookPlayStyle::LOOP:
            idx += fwd;
            if(idx < 0) idx = static_cast<long>(n) - 1;
            else if(idx >= static_cast<long>(n)) idx = 0;
            break;
        case FlipbookPlayStyle::ONCE: {
            const long last = invert ? 0 : static_cast<long>(n) - 1;
            if(idx != last) {
                idx += fwd;
                if(idx < 0) idx = 0;
                else if(idx >= static_cast<long>(n)) idx = static_cast<long>(n) - 1;
            }
            if(idx == last)
                rt.playing = false;   // reached the end -> hold here, stop advancing
            break;
        }
        case FlipbookPlayStyle::PING_PONG: {
            int dir = rt.pingPongReversing ? -fwd : fwd;
            long next = idx + dir;
            if(next < 0 || next >= static_cast<long>(n)) {
                rt.pingPongReversing = !rt.pingPongReversing;
                dir = -dir;
                next = idx + dir;
                if(next < 0) next = 0;
                else if(next >= static_cast<long>(n)) next = static_cast<long>(n) - 1;
            }
            idx = next;
            break;
        }
    }
    rt.frameIndex = static_cast<size_t>(std::clamp<long>(idx, 0, static_cast<long>(n) - 1));
}

void DrawingProgramLayerFolder::flipbook_begin(bool invert) {
    const size_t n = frame_count();
    flipbookRuntime.frameIndex = (invert && n > 0) ? n - 1 : 0;
    flipbookRuntime.frameTimer = 0.0;
    flipbookRuntime.pingPongReversing = false;
    flipbookRuntime.playing = true;
}

void DrawingProgramLayerFolder::flipbook_advance(FlipbookPlayStyle style, bool invert, float fps, float deltaTime) {
    auto& rt = flipbookRuntime;
    const size_t n = frame_count();
    if(!rt.playing || n <= 1 || fps <= 0.0f)
        return;
    const double frameDur = 1.0 / static_cast<double>(fps);
    rt.frameTimer += static_cast<double>(deltaTime);
    // Cap steps so a long stall / tab-out (huge deltaTime) can't spin the whole
    // sequence many times in one frame.
    const int maxSteps = static_cast<int>(n) + 1;
    int steps = 0;
    while(rt.frameTimer >= frameDur && steps < maxSteps) {
        rt.frameTimer -= frameDur;
        ++steps;
        flipbook_step_once(rt, n, style, invert);
        if(!rt.playing)   // ONCE finished this step
            break;
    }
    if(steps >= maxSteps)
        rt.frameTimer = 0.0;   // drop the backlog rather than freeze next frame
}

void DrawingProgramLayerFolder::set_component_list_callbacks(DrawingProgramLayerManager& layerMan) {
    folderList->set_insert_callback([&](auto& c) {
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        c->obj->set_component_list_callbacks(layerMan);
        layerMan.drawP.world.set_to_layout_gui_if_focus();
    });
    folderList->set_erase_callback([&](auto& c) {
        // PERF: deleting a layer/folder only changes the region its content
        // covered — invalidate that footprint (recurses into folders) instead of
        // clear_own_cached_surfaces(), which nuked every cached node surface and
        // forced a full-canvas re-rasterize next frame (a big hitch on large
        // multi-layer files). Done before set_to_erase, while bounds are intact.
        layerMan.drawP.drawCache.invalidate_layer_footprint(&(*c->obj));
        c->obj->set_to_erase();
        layerMan.drawP.world.set_to_layout_gui_if_focus();
    });
    folderList->set_move_callback([&](auto& c, uint32_t oldPos) {
        layerMan.drawP.drawCache.clear_own_cached_surfaces();
        layerMan.drawP.world.set_to_layout_gui_if_focus();
    });
    for(auto& listItem : *folderList)
        listItem.obj->set_component_list_callbacks(layerMan);
}

void DrawingProgramLayerFolder::set_to_erase() {
    for(auto& listItem : *folderList)
        listItem.obj->set_to_erase();
}

void DrawingProgramLayerFolder::get_flattened_component_list(std::vector<CanvasComponentContainer::ObjInfo*>& objList) const {
    for(auto& listItem : (*folderList) | std::views::reverse)
        listItem.obj->get_flattened_component_list(objList);
}

NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> DrawingProgramLayerFolder::get_initial_editing_layer() const {
    // BFS, select a layer that's closest to root as possible
    for(auto& c : *folderList) {
        if(!c.obj->is_folder())
            return c.obj;
    }
    for(auto& c : *folderList) {
        if(c.obj->is_folder()) {
            NetworkingObjects::NetObjWeakPtr<DrawingProgramLayerListItem> toRet = c.obj->get_folder().get_initial_editing_layer();
            if(!toRet.expired())
                return toRet;
        }
    }
    return {};
}

void DrawingProgramLayerFolder::scale_up(const WorldScalar& scaleUpAmount) {
    for(auto& p : *folderList)
        p.obj->scale_up(scaleUpAmount);
}

void DrawingProgramLayerFolder::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version, DrawingProgramLayerManager& layerMan) {
    folderList = layerMan.drawP.world.netObjMan.make_obj<NetworkingObjects::NetObjOrderedList<DrawingProgramLayerListItem>>();
    uint32_t folderListSize;
    a(folderListSize);
    for(uint32_t i = 0; i < folderListSize; i++) {
        DrawingProgramLayerListItem* item = new DrawingProgramLayerListItem();
        item->load_file(a, version, layerMan);
        folderList->push_back_and_send_create(folderList, item);
    }
}

void DrawingProgramLayerFolder::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(folderList->size());
    for(auto& listItem : *folderList)
        listItem.obj->save_file(a);
}

void DrawingProgramLayerFolder::erase_invalid_components() {
    for(auto& listItem : *folderList)
        listItem.obj->erase_invalid_components();
}

void DrawingProgramLayerFolder::get_used_resources(std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const {
    for(auto& listItem : *folderList)
        listItem.obj->get_used_resources(resourceSet);
}
