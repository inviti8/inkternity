#pragma once
#include "CanvasComponent.hpp"
#include "Helpers/SCollision.hpp"
#include <Helpers/NetworkingObjects/NetObjID.hpp>

// PHASE1.md §5 — canvas-side visual proxy for a Waypoint. The data
// (label, framing camera state) lives in WaypointGraph::nodes; this
// component just renders a marker at the drop position and holds the
// NetObjID into the graph so the tool can resolve clicks back to the
// Waypoint that owns the framing rect.
//
// Mirrors BrushStrokeCanvasComponent's structure: the container's
// CoordSpaceHelper places the marker in world space, and markerPos is
// in component-local (== cam-space at drop time) pixels.
//
// M5-a renders a simple filled disc with an outline. Label text and
// selection handles for the framing rect land in M5-b; faint outgoing-
// edge previews when selected land in M5-c.
class WaypointCanvasComponent : public CanvasComponent {
    public:
        // Visual radius of the marker in cam-space pixels. Same intent as
        // the brush-cursor circle in BrushTool.
        static constexpr float MARKER_RADIUS_PX = 10.0f;

        WaypointCanvasComponent();

        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void set_data_from(const CanvasComponent& other) override;

        void set_data(NetworkingObjects::NetObjID waypointId, const Vector2f& markerPos);
        NetworkingObjects::NetObjID get_waypoint_id() const { return d.waypointId; }
        // VECTOR_ERASER/waypoint fix (INFPNT000035): re-point this marker at its node
        // after node ids are reassigned on load (World applies the marker-linkage block).
        void relink_waypoint_id(NetworkingObjects::NetObjID id) { d.waypointId = id; }
        const Vector2f& get_marker_pos() const { return d.markerPos; }

        struct Data {
            NetworkingObjects::NetObjID waypointId{};
            Vector2f markerPos{0.0f, 0.0f};
            template <typename Archive> void serialize(Archive& a) {
                a(waypointId, markerPos);
            }
        } d;

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;
};
