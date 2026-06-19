#pragma once
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include <unordered_map>

struct DrawingProgramCacheBVHNode {
    public:
        SCollision::AABB<WorldScalar> bounds;
        CoordSpaceHelper coords;
        Vector2i resolution;
    private:
        std::vector<CanvasComponentContainer::ObjInfo*> components;
        std::vector<std::shared_ptr<DrawingProgramCacheBVHNode>> children;
        friend class DrawingProgramCache;
};

class DrawingProgramCache {
    public:
        static size_t MINIMUM_COMPONENTS_TO_START_REBUILD;
        static size_t MAXIMUM_COMPONENTS_IN_SINGLE_NODE;
        static size_t MAXIMUM_DRAW_CACHE_SURFACES;
        static size_t CACHE_NODE_RESOLUTION;
        static size_t MILLISECOND_FRAME_TIME_TO_FORCE_CACHE_REFRESH;
        static size_t MILLISECOND_MINIMUM_TIME_TO_CHECK_FORCE_REFRESH;
        // PHASE5.5: while the camera is moving, relax the node-cache gate by up to
        // this many powers of two so coarse upper BVH nodes (which hold "straddler"
        // components) become cacheable+blittable instead of re-rasterized every
        // frame. Blitting a coarse cache upscales (blurry) by up to 2^shift.
        // This is the MAX; the actual shift scales with camera speed (see below),
        // so fast transitions coarsen (smooth, blur invisible) while slow pans stay
        // crisp (shift 0). 0 disables entirely. Tunable live in Settings -> Debug.
        static size_t MOTION_CACHE_COARSEN_SHIFT;
        // Screen-pixels-of-pan-per-frame that adds one level of coarsening. Below
        // this speed the shift is 0 (full quality); each additional this-many px/frame
        // adds one more level up to MOTION_CACHE_COARSEN_SHIFT. Larger = stay crisp
        // at higher speeds (less aggressive). Tunable live in Settings -> Debug.
        static size_t MOTION_CACHE_PX_PER_SHIFT;

        DrawingProgramCache(DrawingProgram& initDrawP);
        void add_component(CanvasComponentContainer::ObjInfo* c);
        void erase_component(CanvasComponentContainer::ObjInfo* c);
        void clear_own_cached_surfaces();
        void preupdate_component(CanvasComponentContainer::ObjInfo* c);
        void build(const std::unordered_set<CanvasComponentContainer::ObjInfo*>& objsToExclude);
        void traverse_bvh_run_function(const SCollision::AABB<WorldScalar>& aabb, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f);
        void traverse_bvh_run_function_starting_at_node(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const SCollision::AABB<WorldScalar>& aabb, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f);
        void traverse_bvh_run_function_starting_at_node_no_collision_check(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>& node)> f);
        void node_loop_erase_if_components(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<bool(CanvasComponentContainer::ObjInfo* comp)> f);
        void node_loop_components(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, std::function<void(CanvasComponentContainer::ObjInfo* comp)> f);
        bool should_rebuild() const;
        // Public for tools that want to make eager rebuild decisions
        // (e.g. EraserTool rebuilds on entry to bound per-segment cost
        // — see EraserTool ctor).
        size_t unsorted_component_count() const { return unsortedComponents.size(); }
        bool check_rebuild_needed_from_framerate();
        void update_and_draw_cached_canvas(SkCanvas* canvas, const DrawData& drawData);
        void draw_components_to_canvas(SkCanvas* canvas, const DrawData& drawData, const std::optional<SCollision::AABB<WorldScalar>>& drawBounds);
        void invalidate_cache_at_aabb(const SCollision::AABB<WorldScalar>& aabb);
        void invalidate_cache_at_optional_aabb(const std::optional<SCollision::AABB<WorldScalar>>& aabb);
        static void delete_all_draw_cache();

        CanvasComponentContainer::ObjInfo* get_front_object_colliding_with_in_editing_layer(const SCollision::ColliderCollection<float>& cC);
        ~DrawingProgramCache();
    private:
        struct NodeCache {
            sk_sp<SkSurface> surface;
            std::chrono::steady_clock::time_point lastRenderTime;
            DrawingProgramCache* attachedDrawingProgramCache;
            std::optional<SCollision::AABB<WorldScalar>> invalidBounds;
        };
        static std::unordered_map<std::shared_ptr<DrawingProgramCacheBVHNode>, NodeCache> nodeCacheMap;

        struct WindowCache {
            sk_sp<SkSurface> surface;
            DrawingProgramCache* attachedDrawingProgramCache = nullptr;
            std::optional<SCollision::AABB<WorldScalar>> invalidBounds;
            CoordSpaceHelper coords;
        };
        static WindowCache windowCache;

        // Effective node-cache gate scale: a node is cacheable/blittable when its
        // coords.inverseScale <= this. Equals the camera scale at rest; coarsened
        // by MOTION_CACHE_COARSEN_SHIFT while the camera is moving.
        WorldScalar cache_gate_scale(const DrawData& drawData) const;

        void refresh_all_draw_cache(const DrawData& drawData);
        void update_window_cache_invalid_bounds(const DrawData& drawData);
        void window_cache_complete_refresh(const DrawData& drawData);
        void allocate_window_cache_area();
        void internal_build(std::vector<CanvasComponentContainer::ObjInfo*> componentsToBuild, const std::unordered_set<CanvasComponentContainer::ObjInfo*>& objsToNotInclude);
        void build_bvh_node(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const std::vector<CanvasComponentContainer::ObjInfo*>& components);
        void build_bvh_node_coords_and_resolution(DrawingProgramCacheBVHNode& node);
        void refresh_draw_cache(const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode, const DrawData& drawData);
        void draw_cache_image_to_canvas(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<DrawingProgramCacheBVHNode>& bvhNode);
        void recursive_draw_layer_item_to_canvas(const DrawingProgramLayerListItem& layerListItem, SkCanvas* canvas, const DrawData& drawData, const std::optional<SCollision::AABB<WorldScalar>>& drawBounds, const std::vector<std::shared_ptr<DrawingProgramCacheBVHNode>>& nodesToDraw);

        std::optional<std::chrono::steady_clock::time_point> badFrametimeTimePoint;
        std::optional<std::chrono::steady_clock::time_point> unorderedObjectsExistTimePoint;

        // PHASE5.5 motion tracking for the coarse-node-cache relaxation.
        CoordSpaceHelper lastWindowCamCoords;
        bool haveLastWindowCamCoords = false;
        bool wasMovingLastFrame = false;
        bool cameraMovingThisFrame = false;
        size_t currentCoarsenShift = 0;   // speed-adaptive: 0 at rest/slow, up to MAX when fast

        std::shared_ptr<DrawingProgramCacheBVHNode> bvhRoot;
        std::vector<CanvasComponentContainer::ObjInfo*> unsortedComponents;
        DrawingProgram& drawP;
};
