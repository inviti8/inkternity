#pragma once
#include "../SharedTypes.hpp"
#include "CanvasComponent.hpp"
#include "../CoordSpaceHelper.hpp"
#include <include/core/SkPath.h>
#include <vector>

class RectangleCanvasComponent : public CanvasComponent {
    public:
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual void change_stroke_color(const Vector4f& newStrokeColor) override;
        virtual std::optional<Vector4f> get_stroke_color() const override;
        virtual CanvasComponentType get_type() const override;
        std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void set_data_from(const CanvasComponent& other) override;
        virtual bool is_mask() const override { return d.isMask; }
        virtual bool is_mask_inverted() const override { return d.maskInvert; }
        virtual std::optional<SkPath> get_mask_path() const override;

        // User input data
        struct Data {
            Vector4f strokeColor;
            Vector4f fillColor;
            float cornerRadius;
            float strokeWidth;
            Vector2f p1;
            Vector2f p2;
            uint8_t fillStrokeMode;
            // PHASE6: polygon mode. When polygonMode is true the shape is the
            // closed polygon defined by `points` (started as the dragged rect's
            // 4 corners; vertices become editable in M2/M3). When false this is
            // the legacy axis-aligned (rounded) rectangle from p1/p2 — old files
            // load with polygonMode=false and are unchanged.
            bool polygonMode = false;
            std::vector<Vector2f> points;
            // PHASE8: optional per-node bezier tangents (parallel to `points`,
            // or empty == every node is a straight corner). controlIn/Out are
            // offsets from the node; an edge A->B is a cubic when A.controlOut or
            // B.controlIn is non-zero, else a straight line. nodeType: 0 corner,
            // 1 smooth (mirrored tangents), 2 cusp (independent). Old polygons
            // load with these empty -> identical straight-edge rendering.
            std::vector<Vector2f> controlIn;
            std::vector<Vector2f> controlOut;
            std::vector<uint8_t> nodeType;
            // PHASE7: when isMask is true this shape is a clip mask for its layer
            // (not drawn as artwork); maskInvert clips to the exterior instead of
            // the interior. Old files default both false (normal shape).
            bool isMask = false;
            bool maskInvert = false;
            bool operator==(const Data& o) const = default;
        } d;

    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        void create_draw_data();
        void create_collider();
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

        SkPath rectPath;

        SCollision::BVHContainer<float> collisionTree;
};

