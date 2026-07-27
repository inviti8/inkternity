#pragma once
#include "CanvasComponent.hpp"
#include "BrushStrokeTessellation.hpp"   // BrushStrokeCanvasComponentPoint + shared tessellation core
#include "Helpers/SCollision.hpp"
#include <Helpers/Serializers.hpp>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>

class BrushStrokeCanvasComponent : public CanvasComponent {
    public:
        constexpr static float DRAW_MINIMUM_LIMIT = 1.0f;

        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void change_stroke_color(const Vector4f& newStrokeColor) override;
        virtual std::optional<Vector4f> get_stroke_color() const override;

        struct Data {
            std::shared_ptr<std::vector<BrushStrokeCanvasComponentPoint>> points = std::make_shared<std::vector<BrushStrokeCanvasComponentPoint>>(); // It's a pointer here since brush strokes cant be edited
            Vector4f color;
            bool hasRoundCaps;
        } d;

        virtual void set_data_from(const CanvasComponent& other) override;
    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual bool accurate_draw(SkCanvas* canvas, const DrawData& drawData, const CoordSpaceHelper& coords, const std::shared_ptr<void>& predrawData) const override;
        virtual std::shared_ptr<void> get_predraw_data_accurate(const DrawData& drawData, const CoordSpaceHelper& coords) const override;

        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const override;
        void create_collider();
        bool should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const override;

        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;

        std::shared_ptr<SkPath> brushPath;
        SCollision::AABB<float> bounds;

        std::array<std::shared_ptr<SkPath>, 2> brushPathLOD;

        std::vector<SCollision::AABB<float>> precheckAABBLevels;
};
