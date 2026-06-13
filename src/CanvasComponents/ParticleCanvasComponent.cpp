#include "ParticleCanvasComponent.hpp"

// timelinefx.h is included only here, behind HVYM_HAS_TIMELINEFX (set by CMake
// on targets that link the desktop-only timelinefx static lib; mirrors
// HVYM_HAS_LIBMYPAINT). Including the header + calling its extern "C" API from
// a C++23 TU is fine — proven by tools/tfx_render_spike.cpp; only *compiling*
// timelinefx.cpp at C++23 tripped the C++20 operator== rule, and that TU stays
// in its own C++17 static lib (deps/timelinefx). On web the component is a
// no-op stub that still round-trips its serialized data.
#ifdef HVYM_HAS_TIMELINEFX
#include "timelinefx.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../DrawCollision.hpp"
#include "CanvasComponentContainer.hpp"
#include "Helpers/ConvertVec.hpp"
#include <Helpers/Logger.hpp>

#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>

#ifdef HVYM_HAS_TIMELINEFX
#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>
#include <include/effects/SkRuntimeEffect.h>
#endif

// ---------------------------------------------------------------------------
#ifdef HVYM_HAS_TIMELINEFX

namespace {

// Faithful port of examples/assets/shaders/timelinefx.frag (docs/design/
// PHASE5.md §M0): per-texel gradient mapping — shape red -> "heat" ->
// color-ramp lookup, then intensity / heat-boost / curved-alpha. Output is
// premultiplied, for additive blending.
const char* kParticleSkSL = R"(
uniform shader uShape;
uniform shader uRamp;
uniform float2 uQuad;
uniform float2 uShapeSize;
uniform float  uRampY;
uniform float  uIntensity;
uniform float  uTexInfluence;
uniform float  uSharpA;
uniform float  uDissolveB;
uniform float3 uHeat;

half4 main(float2 coord) {
    float2 uv = coord / uQuad;
    half4 texel = uShape.eval(uv * uShapeSize);
    float heat = min(pow(texel.r, uHeat.z) * uTexInfluence, 1.0);
    half4 ramp = uRamp.eval(float2(heat * 255.0 + 0.5, uRampY + 0.5));
    ramp.rgb *= 1.0 + uHeat.x * pow(heat, uHeat.y);
    ramp *= uIntensity;
    ramp.a = min(1.0, ramp.a);
    float curved_alpha = 1.0 - smoothstep(texel.a * uDissolveB, texel.a, 1.0 - uSharpA);
    half4 o;
    o.rgb = texel.rgb * ramp.rgb * curved_alpha * texel.a;
    o.a   = texel.a * ramp.a * curved_alpha;
    return o;
}
)";

void ensure_tfx_init() {
    static bool inited = false;
    if (!inited) { tfx_InitialiseTimelineFX(2, 128ull * 1024 * 1024); inited = true; }
}

float snorm16(uint32_t u) {
    int16_t v = static_cast<int16_t>(u & 0xFFFFu);
    return std::max(v / 32767.0f, -1.0f);
}

} // namespace

struct ParticleCanvasComponent::Runtime {
    tfx_library lib = nullptr;
    tfx_effect_manager pm = nullptr;
    tfx_effect_template tmpl = nullptr;
    tfxEffectID id = 0;
    tfx_gpu_particle_properties_t* props = nullptr;
    std::vector<sk_sp<SkImage>> shapes;
    std::vector<sk_sp<SkImage>> ramps;
    sk_sp<SkRuntimeEffect> effect;
    bool loaded = false;
    int emptyFrames = 0;

    static void shape_loader(const char*, tfx_image_data_t* image_data,
                             void* raw, int size, void* user) {
        auto* self = static_cast<Runtime*>(user);
        auto data = SkData::MakeWithCopy(raw, static_cast<size_t>(size));
        sk_sp<SkImage> img;
        if (auto codec = SkPngDecoder::Decode(data, nullptr)) {
            auto [decoded, res] = codec->getImage();
            img = decoded;
        }
        self->shapes.push_back(img);
        image_data->ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(self->shapes.size()));
    }
    static void uv_lookup(void*, tfx_gpu_image_data_t* image_data, int) {
        image_data->uv = {0.f, 0.f, 1.f, 1.f};
        image_data->texture_array_index = 0;
    }
};

void ParticleCanvasComponent::ensure_runtime() const {
    if (rt && rt->loaded) return;
    if (d.packageBytes.empty()) return;
    ensure_tfx_init();
    rt = std::make_unique<Runtime>();

    rt->lib = tfx_LoadEffectLibraryFromMemory(
        d.packageBytes.data(), static_cast<tfxU32>(d.packageBytes.size()),
        &Runtime::shape_loader, &Runtime::uv_lookup, rt.get());
    if (!rt->lib || tfx_GetLibraryErrorStatus(rt->lib) != 0) {
        Logger::get().log("WORLDFATAL", "[Particle] effect package failed to load (error 0x" +
            std::to_string(rt->lib ? tfx_GetLibraryErrorStatus(rt->lib) : 0xFFFFFFFFu) + ")");
        return;
    }
    for (tfxU32 i = 0, n = tfx_GetColorRampBitmapCount(rt->lib); i < n; ++i) {
        tfx_bitmap_t* b = tfx_GetColorRampBitmap(rt->lib, i);
        if (!b || !b->data) { rt->ramps.push_back(nullptr); continue; }
        int rb = b->stride ? b->stride : b->width * b->channels;
        SkImageInfo info = SkImageInfo::Make(b->width, b->height, kRGBA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
        rt->ramps.push_back(SkImages::RasterFromPixmapCopy(SkPixmap(info, b->data, rb)));
    }
    rt->props = static_cast<tfx_gpu_particle_properties_t*>(tfx_ParticlePropertiesBuffer(rt->lib));

    // Resolve which effect to play: the saved name, or the first effect in
    // the package (mirrors the library's own ListEffectNames iteration).
    std::string effName = d.effectName;
    if (effName.empty()) {
        for (tfx_effect_descriptor e : rt->lib->effects) { effName = tfx_GetEffectName(e); break; }
    }
    if (effName.empty()) {
        Logger::get().log("WORLDFATAL", "[Particle] package has no effects");
        return;
    }

    rt->pm = tfx_CreateEffectManager(tfx_CreateEffectManagerInfo(tfxEffectManagerSetup_none));
    rt->tmpl = tfx_CreateEffectTemplate(rt->lib, effName.c_str());
    if (rt->tmpl && tfx_AddEffectTemplateToEffectManager(rt->pm, rt->tmpl, &rt->id))
        tfx_SetEffectPosition(rt->pm, rt->id, 0.f, 0.f, 0.f);

    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(kParticleSkSL));
    if (!effect)
        Logger::get().log("WORLDFATAL", std::string("[Particle] SkSL error: ") + err.c_str());
    rt->effect = effect;
    rt->loaded = rt->effect != nullptr;
}

void ParticleCanvasComponent::update(DrawingProgram& drawP) {
    ensure_runtime();
    if (!rt || !rt->loaded || !rt->pm) return;

    float dtSeconds = static_cast<float>(drawP.world.main.deltaTime);
    tfx_UpdateEffectManager(rt->pm, dtSeconds * 1000.0);

    // Loop the effect: if it has been empty for a beat, re-trigger so the
    // preview keeps playing (one-shot effects like explosions otherwise stop).
    if (tfx_ParticleCount(rt->pm) == 0) {
        if (++rt->emptyFrames > 12 && rt->tmpl) {
            if (tfx_AddEffectTemplateToEffectManager(rt->pm, rt->tmpl, &rt->id))
                tfx_SetEffectPosition(rt->pm, rt->id, 0.f, 0.f, 0.f);
            rt->emptyFrames = 0;
        }
    } else {
        rt->emptyFrames = 0;
    }

    // Animated like a GIF: invalidate this component's cache region each frame
    // (matches ImageCanvasComponent::update; compContainer is set by then).
    drawP.invalidate_cache_at_component(&(*compContainer->objInfo));
}

void ParticleCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData,
                                   const std::shared_ptr<void>& predrawData) const {
    ensure_runtime();
    if (!rt || !rt->loaded || !rt->pm) return;

    tfx_instance_t* inst = tfx_GetInstanceBuffer(rt->pm);
    int count = tfx_GetInstanceCount(rt->pm);
    if (!inst || count <= 0) return;

    const float kIntensityMax = 128.0f / 32767.0f;
    const float s = d.localScale;
    SkSamplingOptions linear(SkFilterMode::kLinear);
    SkSamplingOptions nearest(SkFilterMode::kNearest);

    for (int k = 0; k < count; ++k) {
        const tfx_instance_t& in = inst[k];

        float sz[2];
        tfx_GetSpriteScale(const_cast<tfx_instance_t*>(&in), sz);
        float w = sz[0] * s, h = sz[1] * s;
        if (w <= 0.f || h <= 0.f) continue;

        uint32_t img_idx = in.indexes & 0x1FFFu;
        if (img_idx >= rt->shapes.size() || !rt->shapes[img_idx]) continue;
        const sk_sp<SkImage>& shape = rt->shapes[img_idx];

        uint32_t prop_idx = (in.indexes >> 16) & 0xFFFFu;
        const tfx_gpu_particle_properties_t& pr = rt->props[prop_idx];
        int ramp_array = (pr.color_ramp_indexes >> 8) & 0xFF;
        float ramp_y = static_cast<float>(pr.color_ramp_indexes & 0xFF);
        if (ramp_array >= static_cast<int>(rt->ramps.size()) || !rt->ramps[ramp_array]) continue;
        const sk_sp<SkImage>& ramp = rt->ramps[ramp_array];

        uint32_t lo = static_cast<uint32_t>(in.quaternion & 0xFFFFFFFFull);
        uint32_t hi = static_cast<uint32_t>(in.quaternion >> 32);
        float qx = snorm16(lo), qy = snorm16(lo >> 16), qz = snorm16(hi), qw = snorm16(hi >> 16);
        float ql = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
        if (ql > 1e-6f) { qx/=ql; qy/=ql; qz/=ql; qw/=ql; }
        float c00 = 1.f - 2.f*(qy*qy + qz*qz);
        float c01 = 2.f*(qx*qy + qz*qw);
        float angle_deg = std::atan2(c01, c00) * 180.0f / 3.14159265f;

        SkRuntimeShaderBuilder b(rt->effect);
        b.child("uShape") = shape->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
        b.child("uRamp")  = ramp->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, nearest);
        b.uniform("uQuad")         = SkV2{w, h};
        b.uniform("uShapeSize")    = SkV2{(float)shape->width(), (float)shape->height()};
        b.uniform("uRampY")        = ramp_y;
        b.uniform("uIntensity")    = in.intensity_gradient_map.x * kIntensityMax;
        b.uniform("uTexInfluence") = in.intensity_gradient_map.y * kIntensityMax;
        b.uniform("uSharpA")       = in.curved_alpha_life.x / 255.0f;
        b.uniform("uDissolveB")    = in.curved_alpha_life.y / 255.0f;
        b.uniform("uHeat") = SkV3{pr.heat_response_boost, pr.heat_response_sharpness,
                                  pr.heat_response_curve};

        SkPaint paint;
        paint.setShader(b.makeShader());
        paint.setBlendMode(SkBlendMode::kPlus);  // additive (M1: per-emitter blend mode TODO)

        float hx = pr.image_handle.x, hy = pr.image_handle.y;
        SkMatrix m = SkMatrix::Translate(in.position.x * s, in.position.y * s);
        m.preRotate(angle_deg);
        m.preTranslate(-hx * w, -hy * h);
        canvas->save();
        canvas->concat(m);
        canvas->drawRect(SkRect::MakeWH(w, h), paint);
        canvas->restore();
    }
}

#else // !HVYM_HAS_TIMELINEFX — web/no-timelinefx stub

struct ParticleCanvasComponent::Runtime {};
void ParticleCanvasComponent::ensure_runtime() const {}
void ParticleCanvasComponent::update(DrawingProgram&) {}
void ParticleCanvasComponent::draw(SkCanvas*, const DrawData&, const std::shared_ptr<void>&) const {}

#endif // HVYM_HAS_TIMELINEFX
// ---------------------------------------------------------------------------

ParticleCanvasComponent::ParticleCanvasComponent() = default;
ParticleCanvasComponent::~ParticleCanvasComponent() = default;

CanvasComponentType ParticleCanvasComponent::get_type() const {
    return CanvasComponentType::PARTICLE;
}

void ParticleCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
    create_collider();
    ensure_runtime();
}

void ParticleCanvasComponent::create_collider() {
    using namespace SCollision;
    ColliderCollection<float> objs;
    float r = d.radius;
    std::array<Vector2f, 4> t = triangle_from_rect_points(Vector2f{-r, -r}, Vector2f{r, r});
    objs.triangle.emplace_back(t[0], t[1], t[2]);
    objs.triangle.emplace_back(t[2], t[3], t[0]);
    collisionTree.clear();
    collisionTree.calculate_bvh_recursive(objs);
}

bool ParticleCanvasComponent::collides_within_coords(const SCollision::ColliderCollection<float>& checkAgainst) const {
    return collisionTree.is_collide(checkAgainst);
}

SCollision::AABB<float> ParticleCanvasComponent::get_obj_coord_bounds() const {
    return collisionTree.objects.bounds;
}

std::unique_ptr<CanvasComponent> ParticleCanvasComponent::get_data_copy() const {
    auto c = std::make_unique<ParticleCanvasComponent>();
    c->d = d;
    return c;
}

void ParticleCanvasComponent::set_data_from(const CanvasComponent& other) {
    d = static_cast<const ParticleCanvasComponent&>(other).d;
    rt.reset();  // force rebuild from the new package
}

void ParticleCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    a(d.packageBytes, d.effectName, d.seed, d.localScale, d.radius);
}
void ParticleCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    a(d.packageBytes, d.effectName, d.seed, d.localScale, d.radius);
    rt.reset();
}
void ParticleCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    a(d.packageBytes, d.effectName, d.seed, d.localScale, d.radius);
}
void ParticleCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    a(d.packageBytes, d.effectName, d.seed, d.localScale, d.radius);
    rt.reset();
}
