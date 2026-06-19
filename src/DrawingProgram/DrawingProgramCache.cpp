#include "DrawingProgramCache.hpp"
#include "DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../Diagnostics/RenderStats.hpp"
#include "Helpers/Parallel.hpp"
#include "Layers/DrawingProgramLayerManager.hpp"
#include "Layers/DrawingProgramLayer.hpp"
#include <include/core/SkMatrix.h>
#include <include/pathops/SkPathOps.h>

#ifdef USE_SKIA_BACKEND_GRAPHITE
    #include <include/gpu/graphite/Surface.h>
#elif USE_SKIA_BACKEND_GANESH
    #include <include/gpu/ganesh/GrDirectContext.h>
    #include <include/gpu/ganesh/SkSurfaceGanesh.h>
#endif

// 300 — validated live against a heavy crosshatch file (zynx,
// custom-ink stress test). Picked deliberately; do NOT bump back
// to 1000 without re-reading this whole history.
//
// Timeline:
//   - First-pass lowered this to 100 to combat per-frame redraw
//     cost for raster strokes (MyPaintLayerCanvasComponent::draw
//     was recomposing every tile every frame).
//   - That per-frame cost was then fixed properly — the
//     per-component cached SkImage on MyPaintLayer makes per-frame
//     draw cheap regardless of how many components sit in
//     unsortedComponents. So the threshold no longer needs to be
//     low *for frame time*.
//   - With that fix in, 100 was reverted to 1000 because of an
//     ERASER regression: each eraser segment dirties several
//     components (every overlapping stroke under the cursor),
//     pushing them to unsortedComponents. Tripping the threshold
//     reconstructs the ENTIRE BVH (internal_build walks the full
//     flattened component list, not just the unsorted set) plus
//     re-renders every BVH-node cache surface — a cost that scales
//     with total component count. At 100 the eraser tripped a full
//     rebuild every few segments, so erasing got sluggish as the
//     canvas filled up.
//
// Why 300 (not 1000, not 100): the threshold also gates when the
// BVH node-cache kicks in. Below it, every component draws
// individually each frame (thousands of cheap-but-not-free
// drawImage blits + per-frame predraw computation across all of
// unsortedComponents). Above it, those strokes collapse into a
// handful of consolidated node-cache surfaces. Crosshatching
// generates components fast, so 1000 left the canvas in the
// draw-everything-individually regime far too long. 300 brings the
// node-cache benefit forward enough to keep heavy crosshatch files
// responsive, while staying high enough that eraser-driven rebuild
// thrash stays acceptable (the 100-era regression did not return at
// 300 in testing).
//
// User-tunable live via Settings -> Debug ("Number of components
// to force cache rebuild") and persisted in config.json, so an
// artist can adjust to taste. See docs/design/PERF-INVESTIGATION.md
// for the full investigation.
size_t DrawingProgramCache::MINIMUM_COMPONENTS_TO_START_REBUILD = 300;
size_t DrawingProgramCache::MAXIMUM_COMPONENTS_IN_SINGLE_NODE = 50;
#ifdef __EMSCRIPTEN__
    size_t DrawingProgramCache::MAXIMUM_DRAW_CACHE_SURFACES = 40; // Use less VRAM in web build
#else
    size_t DrawingProgramCache::MAXIMUM_DRAW_CACHE_SURFACES = 96;
#endif
size_t DrawingProgramCache::CACHE_NODE_RESOLUTION = 2048;
size_t DrawingProgramCache::MILLISECOND_FRAME_TIME_TO_FORCE_CACHE_REFRESH = 33; // Around 30FPS
size_t DrawingProgramCache::MILLISECOND_MINIMUM_TIME_TO_CHECK_FORCE_REFRESH = 5000; // Should be a bit long to prevent objects that are being updated, like brush strokes, from constantly refreshing the cache.
size_t DrawingProgramCache::MOTION_CACHE_COARSEN_SHIFT = 0; // PHASE5.5: coarse-node cache relaxation during camera motion (0 = off; on-demand tunable in Settings -> Debug)

std::unordered_map<std::shared_ptr<DrawingProgramCacheBVHNode>, DrawingProgramCache::NodeCache> DrawingProgramCache::nodeCacheMap;
DrawingProgramCache::WindowCache DrawingProgramCache::windowCache;

DrawingProgramCache::DrawingProgramCache(DrawingProgram& initDrawP):
    drawP(initDrawP)
{}

void DrawingProgramCache::add_component(CanvasComponentContainer::ObjInfo* c) {
    unsortedComponents.emplace_back(c);
    invalidate_cache_at_optional_aabb(c->obj->get_world_bounds());
}

void DrawingProgramCache::erase_component(CanvasComponentContainer::ObjInfo* c) {
    auto cacheParentBvhNodeLock = c->obj->cacheParentBvhNode.lock();
    if(cacheParentBvhNodeLock) {
        std::erase(cacheParentBvhNodeLock->components, c);
        c->obj->cacheParentBvhNode.reset();
    }
    else
        std::erase(unsortedComponents, c);
    invalidate_cache_at_optional_aabb(c->obj->get_world_bounds());
}

void DrawingProgramCache::invalidate_cache_at_aabb(const SCollision::AABB<WorldScalar>& aabb) {
    for(auto& [node, nodeCache] : nodeCacheMap) {
        if(nodeCache.attachedDrawingProgramCache == this && SCollision::collide(aabb, node->bounds)) {
            if(nodeCache.invalidBounds.has_value()) {
                auto& iBounds = nodeCache.invalidBounds.value();
                iBounds.include_aabb_in_bounds(aabb);
            }
            else
                nodeCache.invalidBounds = aabb;
        }
    }
    if(SCollision::collide(aabb, drawP.world.drawData.cam.viewingAreaGenerousCollider)) {
        if(windowCache.invalidBounds.has_value()) {
            auto& iBounds = windowCache.invalidBounds.value();
            iBounds.include_aabb_in_bounds(aabb);
        }
        else
            windowCache.invalidBounds = aabb;
    }
}

void DrawingProgramCache::invalidate_cache_at_optional_aabb(const std::optional<SCollision::AABB<WorldScalar>>& aabb) {
    if(aabb.has_value())
        invalidate_cache_at_aabb(aabb.value());
}

bool DrawingProgramCache::should_rebuild() const {
    return (unsortedComponents.size() >= MINIMUM_COMPONENTS_TO_START_REBUILD);
}

bool DrawingProgramCache::check_rebuild_needed_from_framerate() {
    if(drawP.world.main.window.lastFrameTime > std::chrono::milliseconds(MILLISECOND_FRAME_TIME_TO_FORCE_CACHE_REFRESH)) {
        if(!badFrametimeTimePoint)
            badFrametimeTimePoint = std::chrono::steady_clock::now();
        else if(unorderedObjectsExistTimePoint && std::chrono::steady_clock::now() - badFrametimeTimePoint.value() >= std::chrono::milliseconds(MILLISECOND_MINIMUM_TIME_TO_CHECK_FORCE_REFRESH) && std::chrono::steady_clock::now() - unorderedObjectsExistTimePoint.value() >= std::chrono::milliseconds(MILLISECOND_MINIMUM_TIME_TO_CHECK_FORCE_REFRESH)) {
            unorderedObjectsExistTimePoint = std::nullopt;
            badFrametimeTimePoint = std::nullopt;
            return true;
        }
    }
    else
        badFrametimeTimePoint = std::nullopt;

    if(unsortedComponents.empty())
        unorderedObjectsExistTimePoint = std::nullopt;
    else if(!unorderedObjectsExistTimePoint)
        unorderedObjectsExistTimePoint = std::chrono::steady_clock::now();

    return false;
}

void DrawingProgramCache::build(const std::unordered_set<CanvasComponentContainer::ObjInfo*>& objsToExclude) {
    internal_build(drawP.layerMan.get_flattened_component_list(), objsToExclude);
}

void DrawingProgramCache::internal_build(std::vector<CanvasComponentContainer::ObjInfo*> componentsToBuild, const std::unordered_set<CanvasComponentContainer::ObjInfo*>& objsToNotInclude) {
    bvhRoot = std::make_shared<DrawingProgramCacheBVHNode>();
    unsortedComponents.clear();
    std::erase_if(componentsToBuild, [&unsortedComponents = unsortedComponents, &objsToNotInclude](auto& c) {
        if(objsToNotInclude.contains(c))
            return true;
        if(!c->obj->get_world_bounds().has_value()) {
            unsortedComponents.emplace_back(c);
            return true;
        }
        return false;
    });
    clear_own_cached_surfaces();
    build_bvh_node(bvhRoot, componentsToBuild);
}

void DrawingProgramCache::clear_own_cached_surfaces() {
    std::erase_if(nodeCacheMap, [&](auto& nodeCachePair) {
        return nodeCachePair.second.attachedDrawingProgramCache == this;
    });
    if(windowCache.attachedDrawingProgramCache == this) // Don't delete the cache surface. We don't have to reallocate it if the window size didn't change
        windowCache.attachedDrawingProgramCache = nullptr;
}

CanvasComponentContainer::ObjInfo* DrawingProgramCache::get_front_object_colliding_with_in_editing_layer(const SCollision::ColliderCollection<float>& cC) {
    auto cCWorld = drawP.world.drawData.cam.c.collider_to_world<SCollision::ColliderCollection<WorldScalar>, SCollision::ColliderCollection<float>>(cC);
    CanvasComponentContainer::ObjInfo* p = nullptr;
    traverse_bvh_run_function(cCWorld.bounds, [&](const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode) {
        node_loop_components(bvhNode, [&](const auto& c) {
            if(drawP.layerMan.component_passes_layer_selector(c, DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED) && (!p || c->pos >= p->pos) && c->obj->collides_with_world_coords(drawP.world.drawData.cam.c, cCWorld))
                p = c;
        });
        return true;
    });
    return p;
}

void DrawingProgramCache::node_loop_erase_if_components(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<bool(CanvasComponentContainer::ObjInfo* comp)> f) {
    if(bvhNode) {
        std::erase_if(bvhNode->components, [&f](const auto& comp) {
            if(f(comp)) {
                comp->obj->cacheParentBvhNode.reset();
                return true;
            }
            return false;
        });
    }
    else
        std::erase_if(unsortedComponents, f);
}

void DrawingProgramCache::node_loop_components(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<void(CanvasComponentContainer::ObjInfo* comp)> f) {
    if(bvhNode)
        std::for_each(bvhNode->components.begin(), bvhNode->components.end(), f);
    else
        std::for_each(unsortedComponents.begin(), unsortedComponents.end(), f);
}

void DrawingProgramCache::preupdate_component(CanvasComponentContainer::ObjInfo* c) {
    // Can be called even if object isn't in cache yet. In that case, it'll just invalidate the cache at the object's AABB
    auto cacheParentBvhNodeLock = c->obj->cacheParentBvhNode.lock();
    if(cacheParentBvhNodeLock) {
        unsortedComponents.emplace_back(c);
        std::erase(cacheParentBvhNodeLock->components, c);
        c->obj->cacheParentBvhNode.reset();
    }
    invalidate_cache_at_optional_aabb(c->obj->get_world_bounds());
}

void DrawingProgramCache::build_bvh_node(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const std::vector<CanvasComponentContainer::ObjInfo*>& components) {
    if(components.empty())
        return;

    bvhNode->bounds = components.front()->obj->get_world_bounds().value();
    for(auto& c : components)
        bvhNode->bounds.include_aabb_in_bounds(c->obj->get_world_bounds().value());

    build_bvh_node_coords_and_resolution(*bvhNode);

    if(components.size() < MAXIMUM_COMPONENTS_IN_SINGLE_NODE) {
        bvhNode->components = components;
        for(auto& c : bvhNode->components)
            c->obj->cacheParentBvhNode = bvhNode;
        return;
    }

    WorldVec boundsCenter = bvhNode->bounds.center();

    std::array<std::vector<CanvasComponentContainer::ObjInfo*>, 4> parts;

    for(auto& c : components) {
        const auto& cAABB = c->obj->get_world_bounds().value();
        if(cAABB.min.x() < boundsCenter.x() && cAABB.max.x() < boundsCenter.x() && cAABB.min.y() < boundsCenter.y() && cAABB.max.y() < boundsCenter.y())
            parts[0].emplace_back(c);
        else if(cAABB.min.x() > boundsCenter.x() && cAABB.max.x() > boundsCenter.x() && cAABB.min.y() < boundsCenter.y() && cAABB.max.y() < boundsCenter.y())
            parts[1].emplace_back(c);
        else if(cAABB.min.x() < boundsCenter.x() && cAABB.max.x() < boundsCenter.x() && cAABB.min.y() > boundsCenter.y() && cAABB.max.y() > boundsCenter.y())
            parts[2].emplace_back(c);
        else if(cAABB.min.x() > boundsCenter.x() && cAABB.max.x() > boundsCenter.x() && cAABB.min.y() > boundsCenter.y() && cAABB.max.y() > boundsCenter.y())
            parts[3].emplace_back(c);
        else {
            bvhNode->components.emplace_back(c);
            c->obj->cacheParentBvhNode = bvhNode;
        }
    }

    for(auto& p : parts) {
        if(!p.empty())
            build_bvh_node(bvhNode->children.emplace_back(std::make_shared<DrawingProgramCacheBVHNode>()), p);
    }
}

void DrawingProgramCache::build_bvh_node_coords_and_resolution(DrawingProgramCacheBVHNode& node) {
    // These are part of the draw cache data, but we're putting it out here, because:
    //  - We dont have to recalculate it every time we refresh the cache
    //  - Some of the data here is needed to determine whether to generate the cache in the first place

    WorldVec cacheBoundDim = node.bounds.dim();
    if(cacheBoundDim.x() > cacheBoundDim.y()) {
        node.resolution.x() = CACHE_NODE_RESOLUTION;
        node.resolution.y() = CACHE_NODE_RESOLUTION * static_cast<double>(cacheBoundDim.y() / cacheBoundDim.x());
    }
    else {
        node.resolution.y() = CACHE_NODE_RESOLUTION;
        node.resolution.x() = CACHE_NODE_RESOLUTION * static_cast<double>(cacheBoundDim.x() / cacheBoundDim.y());
    }
    node.coords.rotation = 0.0;
    node.coords.pos = node.bounds.min;
    // Not necessary, but we choose the number that had less operations done on it for more accuracy
    if(node.resolution.x() > node.resolution.y())
        node.coords.inverseScale = cacheBoundDim.x().divide_double(node.resolution.x());
    else 
        node.coords.inverseScale = cacheBoundDim.y().divide_double(node.resolution.y());
}

void DrawingProgramCache::refresh_all_draw_cache(const DrawData& drawData) {
    const WorldScalar gateScale = cache_gate_scale(drawData);
    traverse_bvh_run_function(drawData.cam.viewingAreaGenerousCollider, [&](std::shared_ptr<DrawingProgramCacheBVHNode> node) {
        if(node && node->coords.inverseScale <= gateScale) {
            refresh_draw_cache(node, drawData);
            return false;
        }
        return true;
    });
}

void DrawingProgramCache::traverse_bvh_run_function(const SCollision::AABB<WorldScalar>& aabb, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f) {
    f(nullptr);
    traverse_bvh_run_function_starting_at_node(bvhRoot, aabb, f);
}

void DrawingProgramCache::traverse_bvh_run_function_starting_at_node(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const SCollision::AABB<WorldScalar>& aabb, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f) {
    if(bvhNode && SCollision::collide(aabb, bvhNode->bounds) && f(bvhNode)) {
        for(auto& p : bvhNode->children)
            traverse_bvh_run_function_starting_at_node(p, aabb, f);
    }
}

void DrawingProgramCache::traverse_bvh_run_function_starting_at_node_no_collision_check(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f) {
    if(bvhNode && f(bvhNode)) {
        for(auto& p : bvhNode->children)
            traverse_bvh_run_function_starting_at_node_no_collision_check(p, f);
    }
}

void DrawingProgramCache::refresh_draw_cache(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const DrawData& drawData) {
    if(bvhNode->children.empty() && bvhNode->components.empty())
        return;

    NodeCache nodeCache;
    auto bvhNodeCacheIt = nodeCacheMap.find(bvhNode);
    if(bvhNodeCacheIt != nodeCacheMap.end()) {
        nodeCache = bvhNodeCacheIt->second;
        if(!nodeCache.invalidBounds.has_value()) // Node is cached already, and doesn't have invalid areas, so there's nothing to do
            return;
        nodeCacheMap.erase(bvhNode); // Ensure nodeCache is erased, so that the draw_components_to_canvas function doesnt use it while drawing
    }
    else
        nodeCache.surface = drawP.world.main.create_native_surface(bvhNode->resolution, true);

    ++RenderStats::get().live.nodeRebuilds;   // actual node-cache (re)render this frame (F2 / cache miss)

    SkCanvas* cacheCanvas = nodeCache.surface->getCanvas();

    DrawData cacheDrawData = drawData;
    cacheDrawData.cam.c = bvhNode->coords;
    cacheDrawData.cam.set_viewing_area(bvhNode->resolution.cast<float>());
    cacheDrawData.refresh_draw_optimizing_values();

    if(nodeCache.invalidBounds.has_value()) {
        auto iBounds = nodeCache.invalidBounds.value();
        iBounds = bvhNode->bounds.get_intersection_between_aabbs(iBounds);

        WorldVec bDim = bvhNode->bounds.dim();

        Vector2f clipBoundMin{static_cast<float>((iBounds.min.x() - bvhNode->bounds.min.x()) / bDim.x()) * bvhNode->resolution.x(),
                              static_cast<float>((iBounds.min.y() - bvhNode->bounds.min.y()) / bDim.y()) * bvhNode->resolution.y()};
        Vector2f clipBoundMax{static_cast<float>((iBounds.max.x() - bvhNode->bounds.min.x()) / bDim.x()) * bvhNode->resolution.x(),
                              static_cast<float>((iBounds.max.y() - bvhNode->bounds.min.y()) / bDim.y()) * bvhNode->resolution.y()};

        SCollision::AABB<float> clipRectBoundAABB{clipBoundMin - Vector2f{4, 4}, clipBoundMax + Vector2f{4, 4}};
        SkIRect clipRect = SkIRect::MakeLTRB(clipRectBoundAABB.min.x(),
                                             clipRectBoundAABB.min.y(),
                                             clipRectBoundAABB.max.x(),
                                             clipRectBoundAABB.max.y());
        SCollision::AABB<WorldScalar> clipRectBoundAABBWorld{
            bvhNode->coords.from_space(clipRectBoundAABB.min),
            bvhNode->coords.from_space(clipRectBoundAABB.max)
        };

        cacheCanvas->save();
        cacheCanvas->clipIRect(clipRect);
        cacheCanvas->clear(SkColor4f{0, 0, 0, 0});
        draw_components_to_canvas(cacheCanvas, cacheDrawData, clipRectBoundAABBWorld);
        cacheCanvas->restore();
        nodeCache.invalidBounds = std::nullopt;
    }
    else {
        cacheCanvas->clear(SkColor4f{0, 0, 0, 0});
        draw_components_to_canvas(cacheCanvas, cacheDrawData, std::nullopt);
    }

    nodeCache.lastRenderTime = std::chrono::steady_clock::now();
    nodeCache.attachedDrawingProgramCache = this;

    while(nodeCacheMap.size() >= MAXIMUM_DRAW_CACHE_SURFACES) {
        auto leastUsedNodeIt = nodeCacheMap.begin();
        for(auto it = nodeCacheMap.begin(); it != nodeCacheMap.end(); ++it) {
            if(it->second.lastRenderTime < leastUsedNodeIt->second.lastRenderTime)
                leastUsedNodeIt = it;
        }
        nodeCacheMap.erase(leastUsedNodeIt);
    }

    nodeCacheMap.emplace(bvhNode, nodeCache); // Set the nodeCache after rendering is done, so that the draw function doesnt assume we have this node cached
}

void DrawingProgramCache::allocate_window_cache_area() {
    const Vector2i& windowSize = drawP.world.main.window.size;
    windowCache.surface = drawP.world.main.create_native_surface(windowSize, true);
}

void DrawingProgramCache::update_window_cache_invalid_bounds(const DrawData& drawData) {
    auto invalidBound = windowCache.invalidBounds.value();
    if(SCollision::collide(invalidBound, drawData.cam.viewingAreaGenerousCollider)) {
        auto iBounds = drawData.cam.viewingAreaGenerousCollider.get_intersection_between_aabbs(invalidBound);

        SCollision::AABB<float> cameraSpaceInvalidAABB = drawData.cam.c.world_collider_to_coords<SCollision::AABB<float>>(iBounds);
        SCollision::AABB<float> clipRectBoundAABB{cameraSpaceInvalidAABB.min - Vector2f{4, 4}, cameraSpaceInvalidAABB.max + Vector2f{4, 4}};

        SkIRect clipRect = SkIRect::MakeLTRB(clipRectBoundAABB.min.x(),
                                             clipRectBoundAABB.min.y(),
                                             clipRectBoundAABB.max.x(),
                                             clipRectBoundAABB.max.y());

        SCollision::AABB<WorldScalar> clipRectBoundAABBWorld = drawData.cam.c.collider_to_world<SCollision::AABB<WorldScalar>>(clipRectBoundAABB);

        SkCanvas* cacheCanvas = windowCache.surface->getCanvas();
        cacheCanvas->save();
        cacheCanvas->clipIRect(clipRect);
        cacheCanvas->clear(SkColor4f{0, 0, 0, 0});
        draw_components_to_canvas(cacheCanvas, drawData, clipRectBoundAABBWorld);
        cacheCanvas->restore();
    }
    windowCache.invalidBounds = std::nullopt;
}

void DrawingProgramCache::window_cache_complete_refresh(const DrawData& drawData) {
    RenderStats::get().live.windowCacheRebuilt = true;   // F1 signal: full-window recomposite this frame
    SkCanvas* cacheCanvas = windowCache.surface->getCanvas();
    cacheCanvas->save();
    cacheCanvas->clear(SkColor4f{0, 0, 0, 0});
    draw_components_to_canvas(cacheCanvas, drawData, std::nullopt);
    cacheCanvas->restore();
    windowCache.attachedDrawingProgramCache = this;
    windowCache.coords = drawData.cam.c;
}

WorldScalar DrawingProgramCache::cache_gate_scale(const DrawData& drawData) const {
    if(cameraMovingThisFrame && MOTION_CACHE_COARSEN_SHIFT > 0)
        return drawData.cam.c.inverseScale << MOTION_CACHE_COARSEN_SHIFT;
    return drawData.cam.c.inverseScale;
}

void DrawingProgramCache::update_and_draw_cached_canvas(SkCanvas* canvas, const DrawData& drawData) {
    RenderStats::get().live.unsortedCount = static_cast<int>(unsortedComponents.size());
    RenderStats::get().live.nodeCacheCount = static_cast<int>(nodeCacheMap.size());

    // PHASE5.5: is the camera moving this frame? Drives the coarse-node cache
    // relaxation (cheap blurry blits in motion) and the crisp rebuild on settle.
    cameraMovingThisFrame = !haveLastWindowCamCoords || drawData.cam.c != lastWindowCamCoords;
    const bool justSettled = !cameraMovingThisFrame && wasMovingLastFrame;
    lastWindowCamCoords = drawData.cam.c;
    haveLastWindowCamCoords = true;
    wasMovingLastFrame = cameraMovingThisFrame;

    if(windowCache.surface == nullptr) {
        allocate_window_cache_area();
        refresh_all_draw_cache(drawData);
        window_cache_complete_refresh(drawData);
    }
    // justSettled forces one crisp rebuild even though the camera pose now matches
    // the (motion-built, blurry) window cache, replacing it with a sharp one.
    else if(windowCache.attachedDrawingProgramCache != this || drawData.cam.c != windowCache.coords || justSettled) {
        refresh_all_draw_cache(drawData);
        window_cache_complete_refresh(drawData);
    }
    else if(windowCache.invalidBounds.has_value()) {
        refresh_all_draw_cache(drawData);
        update_window_cache_invalid_bounds(drawData);
    }
    canvas->drawImage(windowCache.surface->makeTemporaryImage(), 0, 0, {SkFilterMode::kNearest, SkMipmapMode::kNone}, nullptr);
}

void DrawingProgramCache::draw_components_to_canvas(SkCanvas* canvas, const DrawData& drawData, const std::optional<SCollision::AABB<WorldScalar>>& drawBounds) {
    ++RenderStats::get().live.treeWalks;   // each call = one full layer-tree walk this frame
    if(drawP.layerMan.layer_tree_root_exists()) {
        std::vector<std::shared_ptr<DrawingProgramCacheBVHNode>> cachedNodesToDraw;
        std::vector<std::shared_ptr<DrawingProgramCacheBVHNode>> uncachedNodes;

        const WorldScalar gateScale = cache_gate_scale(drawData);
        traverse_bvh_run_function(drawData.cam.viewingAreaGenerousCollider, [&](const std::shared_ptr<DrawingProgramCacheBVHNode>& node) {
            if(node) {
                auto it = nodeCacheMap.find(node);
                if(it != nodeCacheMap.end()) {
                    auto& nodeCache = it->second;
                    if(!nodeCache.invalidBounds.has_value() && node->coords.inverseScale <= gateScale) {
                        cachedNodesToDraw.emplace_back(node);
                        return false;
                    }
                }
            }
            if(node && node->coords.inverseScale > (drawData.cam.c.inverseScale >> CanvasComponentContainer::COMP_MIN_SHIFT_BEFORE_DISAPPEAR)) // Not the "unsorted components" node (which is nullptr)
                uncachedNodes.emplace_back(node);
            else
                return false;
            return true;
        });

        recursive_draw_layer_item_to_canvas(drawP.layerMan.get_layer_root(), canvas, drawData, drawBounds, uncachedNodes);

        for(auto& nodeCacheToDraw : cachedNodesToDraw)
            draw_cache_image_to_canvas(canvas, drawData, nodeCacheToDraw);
    }
}

void DrawingProgramCache::recursive_draw_layer_item_to_canvas(const DrawingProgramLayerListItem& layerListItem, SkCanvas* canvas, const DrawData& drawData, const std::optional<SCollision::AABB<WorldScalar>>& drawBounds, const std::vector<std::shared_ptr<DrawingProgramCacheBVHNode>>& nodesToDraw) {
    if(layerListItem.get_visible()) {
        SkPaint layerPaint;
        layerPaint.setAlphaf(layerListItem.get_alpha());
        layerPaint.setBlendMode(serialized_blend_mode_to_sk_blend_mode(layerListItem.get_blend_mode()));
        canvas->saveLayer(nullptr, &layerPaint);
        ++RenderStats::get().live.saveLayersIssued;   // F7.1 signal: isolation buffer opened per visible layer
        if(layerListItem.is_folder()) {
            for(auto& p : *layerListItem.get_folder().folderList | std::views::reverse)
                recursive_draw_layer_item_to_canvas(*p.obj, canvas, drawData, drawBounds, nodesToDraw);
        }
        else {
            std::vector<CanvasComponentContainer::ObjInfo*> compsToDraw;
            // PHASE7: mask shapes are not drawn as content (is_mask) — they only
            // define the clip applied below. Exclude them from the predraw set.
            parallel_loop_container(nodesToDraw, [&](auto& node) {
                std::for_each(node->components.begin(), node->components.end(), [&](auto& c) {
                    if(c->obj->parentLayer == &layerListItem && !c->obj->get_comp().is_mask() && (!drawBounds.has_value() || SCollision::collide(drawBounds.value(), c->obj->get_world_bounds().value())) && c->obj->should_draw(drawData))
                        c->obj->preDrawDataHolder = c->obj->calculate_predraw_data(drawData);
                    else
                        c->obj->preDrawDataHolder = std::nullopt;
                });
            });
            parallel_loop_container(unsortedComponents, [&](auto& c) {
                if(c->obj->parentLayer == &layerListItem && !c->obj->get_comp().is_mask() && c->obj->get_world_bounds().has_value() && (!drawBounds.has_value() || SCollision::collide(drawBounds.value(), c->obj->get_world_bounds().value())) && c->obj->should_draw(drawData))
                    c->obj->preDrawDataHolder = c->obj->calculate_predraw_data(drawData);
                else
                    c->obj->preDrawDataHolder = std::nullopt;
            });
            for(auto& node : nodesToDraw) {
                for(auto& c : node->components) {
                    if(c->obj->preDrawDataHolder.has_value())
                        compsToDraw.emplace_back(c);
                }
            }
            for(auto& c : unsortedComponents) {
                if(c->obj->preDrawDataHolder.has_value())
                    compsToDraw.emplace_back(c);
            }
            std::sort(compsToDraw.begin(), compsToDraw.end(), [](auto& a, auto& b) {
                return a->pos < b->pos;
            });
            // F7 signal: a visible leaf layer with nothing on screen still paid a
            // saveLayer above (visibleLayers - visibleLayersInView == wasted buffers).
            RenderStats::Frame& stats = RenderStats::get().live;
            ++stats.visibleLayers;
            if(!compsToDraw.empty())
                ++stats.visibleLayersInView;
            stats.directComponentDraws += static_cast<int>(compsToDraw.size());
            // PHASE7: clip the layer's content to its mask shapes (scoped by the
            // saveLayer above; restored with the canvas->restore() below).
            apply_layer_mask_clip(layerListItem, canvas, drawData);
            for(auto& c : compsToDraw)
                c->obj->draw_with_predraw_data(canvas, drawData, c->obj->preDrawDataHolder.value());
        }
        canvas->restore();
    }
}

void DrawingProgramCache::apply_layer_mask_clip(const DrawingProgramLayerListItem& layerListItem, SkCanvas* canvas, const DrawData& drawData) {
    auto& components = layerListItem.get_layer().components;
    if(!components) return;

    // Gather mask shapes across the WHOLE layer (an off-screen mask still clips
    // on-screen content). Each shape's path is component-local; map it to draw
    // space with the same scale->rotate->translate the normal draw uses.
    std::vector<SkPath> normalMasks;
    std::vector<SkPath> invMasks;
    for(auto& p : *components) {
        CanvasComponentContainer& container = *p.obj;
        CanvasComponent& comp = container.get_comp();
        if(!comp.is_mask()) continue;
        std::optional<SkPath> localPath = comp.get_mask_path();
        if(!localPath.has_value()) continue;
        const CanvasComponentContainer::TransformData td = container.calculate_draw_transform(drawData);
        SkMatrix m;
        m.setScale(td.scale, td.scale);
        m.preRotate(td.rotation);
        m.preTranslate(td.translation.x(), td.translation.y());
        SkPath worldPath = localPath->makeTransform(m);
        if(comp.is_mask_inverted())
            invMasks.emplace_back(std::move(worldPath));
        else
            normalMasks.emplace_back(std::move(worldPath));
    }
    if(normalMasks.empty() && invMasks.empty()) return;

    // region = union(non-inverted) intersected with the canvas, then each
    // inverted mask subtracted (knocks a hole). Only-inverted -> whole layer
    // minus the holes. (See PHASE7.md composition rules.)
    if(!normalMasks.empty()) {
        SkPath unionPath = normalMasks[0];
        for(size_t i = 1; i < normalMasks.size(); ++i)
            Op(unionPath, normalMasks[i], kUnion_SkPathOp, &unionPath);
        canvas->clipPath(unionPath, SkClipOp::kIntersect, true);
    }
    for(const SkPath& iv : invMasks)
        canvas->clipPath(iv, SkClipOp::kDifference, true);
}

void DrawingProgramCache::draw_cache_image_to_canvas(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode) {
    auto it = nodeCacheMap.find(bvhNode);
    if(it != nodeCacheMap.end()) {
        auto& nodeCache = it->second;
        ++RenderStats::get().live.cachedNodeBlits;
        canvas->save();
        bvhNode->coords.transform_sk_canvas(canvas, drawData);
        nodeCache.lastRenderTime = std::chrono::steady_clock::now();

        SkPaint srcPaint;
        srcPaint.setBlendMode(SkBlendMode::kSrc);

        canvas->drawImage(nodeCache.surface->makeTemporaryImage(), 0, 0, {SkFilterMode::kLinear, SkMipmapMode::kLinear}, &srcPaint);

        canvas->restore();
    }
}

void DrawingProgramCache::delete_all_draw_cache() {
    windowCache.surface = nullptr;
    windowCache.attachedDrawingProgramCache = nullptr;
    nodeCacheMap.clear();
}

DrawingProgramCache::~DrawingProgramCache() {
    clear_own_cached_surfaces();
}
