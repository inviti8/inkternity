// M0 render proof for PHASE5 particle systems (docs/design/PHASE5.md).
//
// Loads a .tfx effect package, runs TimelineFX's CPU simulation for N frames,
// then renders the resulting instance buffer with a faithful SkRuntimeEffect
// port of timelinefx.frag (the per-texel color-ramp / gradient-mapping path
// chosen over plain drawAtlas). Writes a PNG of the frame.
//
// Pipeline per particle (all driven from CPU-read TimelineFX data):
//   * position  = instance.position.xy (plain float world coords)
//   * size      = tfx_GetSpriteScale(instance)
//   * rotation  = 16-bit snorm quaternion -> in-plane angle
//   * image     = tfx_GetSpriteImagePointer(pm, instance.indexes) -> SkImage
//   * properties= tfx_ParticlePropertiesBuffer[instance.indexes >> 16]
//                 (color-ramp row, heat response, image handle)
//   * intensity/gradient = instance.intensity_gradient_map (u16 * 128/32767)
//   * sharpness/dissolve = instance.curved_alpha_life (unorm8)
//   * color     = SkSL port of timelinefx.frag: shape texel.r -> "heat" ->
//                 color-ramp lookup, intensity + curved-alpha compositing.
//
// Build (EXCLUDE_FROM_ALL, conan builds only):
//   cmake --build build --config Release --target tfx_render_spike
// Run:
//   build/Release/tfx_render_spike.exe build/tfx_test/effects.tfx out.png \
//       "Vader Explosion" 24 1.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "timelinefx.h"

#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/encode/SkPngEncoder.h>

namespace {

std::vector<sk_sp<SkImage>> g_shapes;  // 1-based index stashed on image_data->ptr

// Faithful port of examples/assets/shaders/timelinefx.frag (the billboard
// fragment shader). coord arrives in the quad's local pixel space (0..quad);
// we map to the shape's uv, sample the shape "heat" (red channel), look up the
// per-emitter color ramp, then apply intensity, heat boost and the curved
// alpha. Output is premultiplied (rgb already * alpha) for additive blending.
const char* kParticleSkSL = R"(
uniform shader uShape;       // particle shape image (sampled in its pixel space)
uniform shader uRamp;        // color-ramp image (256 wide; rows = emitters)
uniform float2 uQuad;        // local quad size (coord range)
uniform float2 uShapeSize;   // shape image dimensions in px
uniform float  uRampY;       // ramp row for this emitter (px)
uniform float  uIntensity;   // color multiplier (igm.x * 128/32767)
uniform float  uTexInfluence;// gradient-map value  (igm.y * 128/32767)
uniform float  uSharpA;      // curved_alpha_life.x  (frag .y term)
uniform float  uDissolveB;   // curved_alpha_life.y  (frag .z term)
uniform float3 uHeat;        // boost, sharpness, curve

half4 main(float2 coord) {
    float2 uv = coord / uQuad;
    half4 texel = uShape.eval(uv * uShapeSize);

    float heat = min(pow(texel.r, uHeat.z) * uTexInfluence, 1.0);
    float ramp_x = heat * 255.0;
    half4 ramp = uRamp.eval(float2(ramp_x + 0.5, uRampY + 0.5));
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

bool write_png(const sk_sp<SkImage>& img, const std::string& path) {
    if (!img) return false;
    sk_sp<SkImage> raster = img->makeRasterImage();
    SkPixmap pm;
    if (!raster || !raster->peekPixels(&pm)) return false;
    SkFILEWStream out(path.c_str());
    return out.isValid() && SkPngEncoder::Encode(&out, pm, SkPngEncoder::Options{});
}

void skia_shape_loader(const char* filename, tfx_image_data_t* image_data,
                       void* raw_image_data, int image_memory_size, void* user_data) {
    (void)filename; (void)user_data;
    auto data = SkData::MakeWithCopy(raw_image_data, static_cast<size_t>(image_memory_size));
    std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(data, nullptr);
    sk_sp<SkImage> img;
    if (codec) { auto [decoded, res] = codec->getImage(); img = decoded; }
    g_shapes.push_back(img);
    image_data->ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(g_shapes.size()));
}

void uv_lookup(void* ptr, tfx_gpu_image_data_t* image_data, int offset) {
    (void)ptr; (void)offset;
    image_data->uv = {0.f, 0.f, 1.f, 1.f};
    image_data->texture_array_index = 0;
}

float snorm16(uint32_t u) {
    int16_t v = static_cast<int16_t>(u & 0xFFFFu);
    return std::max(v / 32767.0f, -1.0f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: tfx_render_spike <effects.tfx> <out.png> "
                    "[effect_name] [frames] [scale] [canvas_px]\n");
        return 2;
    }
    const char* path = argv[1];
    const std::string out_png = argv[2];
    const char* effect_name = argc > 3 ? argv[3] : "Vader Explosion";
    const int frames = argc > 4 ? std::atoi(argv[4]) : 24;
    const float scale = argc > 5 ? static_cast<float>(std::atof(argv[5])) : 0.0f; // 0 = auto-fit
    const int canvas_px = argc > 6 ? std::atoi(argv[6]) : 768;

    tfx_InitialiseTimelineFX(1, 128ull * 1024 * 1024);
    if (tfx_ValidateEffectPackage(path) != 0) {
        std::printf("[render] invalid package: %s\n", path);
        return 1;
    }
    tfx_library lib = tfx_LoadEffectLibrary(path, skia_shape_loader, uv_lookup, nullptr);
    if (!lib || tfx_GetLibraryErrorStatus(lib) != 0) {
        std::printf("[render] library load error 0x%X\n",
                    lib ? tfx_GetLibraryErrorStatus(lib) : 0xFFFFFFFFu);
        return 1;
    }
    std::printf("[render] loaded %zu shapes\n", g_shapes.size());

    // Color-ramp bitmaps -> SkImages (one per bitmap; rows index emitters).
    std::vector<sk_sp<SkImage>> ramps;
    for (tfxU32 i = 0, n = tfx_GetColorRampBitmapCount(lib); i < n; ++i) {
        tfx_bitmap_t* b = tfx_GetColorRampBitmap(lib, i);
        if (!b || !b->data) { ramps.push_back(nullptr); continue; }
        int rb = b->stride ? b->stride : b->width * b->channels;
        SkImageInfo info = SkImageInfo::Make(b->width, b->height, kRGBA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
        ramps.push_back(SkImages::RasterFromPixmapCopy(SkPixmap(info, b->data, rb)));
    }

    auto* props = static_cast<tfx_gpu_particle_properties_t*>(tfx_ParticlePropertiesBuffer(lib));

    // Simulate.
    tfx_effect_manager pm = tfx_CreateEffectManager(
        tfx_CreateEffectManagerInfo(tfxEffectManagerSetup_none));
    tfx_effect_template tmpl = tfx_CreateEffectTemplate(lib, effect_name);
    if (!tmpl) { std::printf("[render] no effect '%s'\n", effect_name); return 1; }
    tfxEffectID id;
    if (!tfx_AddEffectTemplateToEffectManager(pm, tmpl, &id)) {
        std::printf("[render] could not add effect\n"); return 1;
    }
    tfx_SetEffectPosition(pm, id, 0.f, 0.f, 0.f);
    for (int f = 0; f < frames; ++f) tfx_UpdateEffectManager(pm, 1000.0 / 60.0);

    tfx_instance_t* inst = tfx_GetInstanceBuffer(pm);
    int count = tfx_GetInstanceCount(pm);
    std::printf("[render] frame %d: %d instances (%u particles)\n",
                frames, count, tfx_ParticleCount(pm));

    // Auto-fit world->pixel scale from the particle extent (effect world units
    // are arbitrary). Extent = max(|x|,|y|) + half the largest sprite.
    float extent = 0.f;
    for (int k = 0; k < count; ++k) {
        float s[2]; tfx_GetSpriteScale(const_cast<tfx_instance_t*>(&inst[k]), s);
        extent = std::max(extent, std::max(std::fabs(inst[k].position.x),
                                           std::fabs(inst[k].position.y))
                                      + 0.5f * std::max(s[0], s[1]));
    }
    float fit_scale = (scale > 0.f) ? scale
                                    : (extent > 1e-4f ? (canvas_px * 0.42f) / extent : 1.f);
    std::printf("[render] extent=%.3f -> scale=%.2f\n", extent, fit_scale);

    // Build the runtime effect once.
    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(kParticleSkSL));
    if (!effect) { std::printf("[render] SkSL error: %s\n", err.c_str()); return 1; }

    // Raster surface, black background, additive blending.
    sk_sp<SkSurface> surface = SkSurfaces::Raster(
        SkImageInfo::MakeN32Premul(canvas_px, canvas_px));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorBLACK);
    const float cx = canvas_px * 0.5f, cy = canvas_px * 0.5f;

    const float kIntensityMax = 128.0f / 32767.0f; // intensity_gradient_map u16 scale

    SkSamplingOptions linear(SkFilterMode::kLinear);
    int drawn = 0;
    for (int k = 0; k < count; ++k) {
        const tfx_instance_t& in = inst[k];

        float sz[2];
        tfx_GetSpriteScale(const_cast<tfx_instance_t*>(&in), sz);
        float w = sz[0] * fit_scale, h = sz[1] * fit_scale;
        if (w <= 0.f || h <= 0.f) continue;

        // Image index is encoded in the instance; g_shapes is in load order,
        // which matches the GPU shape array the shader indexes.
        uint32_t img_idx = in.indexes & 0x1FFFu;
        if (img_idx >= g_shapes.size()) continue;
        const sk_sp<SkImage>& shape = g_shapes[img_idx];
        if (!shape) continue;

        uint32_t prop_idx = (in.indexes >> 16) & 0xFFFFu;
        const tfx_gpu_particle_properties_t& pr = props[prop_idx];
        int ramp_array = (pr.color_ramp_indexes >> 8) & 0xFF;
        float ramp_y = static_cast<float>(pr.color_ramp_indexes & 0xFF);
        const sk_sp<SkImage>& ramp =
            (ramp_array < static_cast<int>(ramps.size())) ? ramps[ramp_array] : ramps.empty() ? nullptr : ramps[0];
        if (!ramp) continue;

        // Rotation: snorm quaternion -> in-plane angle.
        uint32_t lo = static_cast<uint32_t>(in.quaternion & 0xFFFFFFFFull);
        uint32_t hi = static_cast<uint32_t>(in.quaternion >> 32);
        float qx = snorm16(lo), qy = snorm16(lo >> 16), qz = snorm16(hi), qw = snorm16(hi >> 16);
        float ql = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
        if (ql > 1e-6f) { qx/=ql; qy/=ql; qz/=ql; qw/=ql; }
        // rotation-matrix column 0 (matches QuaternionToRotationMatrix in the vert)
        float c00 = 1.f - 2.f*(qy*qy + qz*qz);
        float c01 = 2.f*(qx*qy + qz*qw);
        float angle_deg = std::atan2(c01, c00) * 180.0f / 3.14159265f;

        // Per-particle unpacked scalars.
        float intensity = in.intensity_gradient_map.x * kIntensityMax;
        float tex_influence = in.intensity_gradient_map.y * kIntensityMax;
        float sharpA = in.curved_alpha_life.x / 255.0f;
        float dissolveB = in.curved_alpha_life.y / 255.0f;
        float hx = pr.image_handle.x, hy = pr.image_handle.y;

        SkRuntimeShaderBuilder b(effect);
        b.child("uShape") = shape->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
        b.child("uRamp")  = ramp->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                             SkSamplingOptions(SkFilterMode::kNearest));
        b.uniform("uQuad")       = SkV2{w, h};
        b.uniform("uShapeSize")  = SkV2{(float)shape->width(), (float)shape->height()};
        b.uniform("uRampY")      = ramp_y;
        b.uniform("uIntensity")  = intensity;
        b.uniform("uTexInfluence") = tex_influence;
        b.uniform("uSharpA")     = sharpA;
        b.uniform("uDissolveB")  = dissolveB;
        b.uniform("uHeat") = SkV3{pr.heat_response_boost, pr.heat_response_sharpness,
                                  pr.heat_response_curve};

        SkPaint paint;
        paint.setShader(b.makeShader());
        paint.setBlendMode(SkBlendMode::kPlus);  // additive

        SkMatrix m = SkMatrix::Translate(cx + in.position.x * fit_scale,
                                         cy + in.position.y * fit_scale);
        m.preRotate(angle_deg);
        m.preTranslate(-hx * w, -hy * h);
        canvas->save();
        canvas->concat(m);
        canvas->drawRect(SkRect::MakeWH(w, h), paint);
        canvas->restore();
        ++drawn;
    }
    std::printf("[render] drew %d / %d instances\n", drawn, count);

    if (write_png(surface->makeImageSnapshot(), out_png))
        std::printf("[render] wrote %s\n", out_png.c_str());
    else
        std::printf("[render] FAILED to write %s\n", out_png.c_str());

    tfx_EndTimelineFX();
    return 0;
}
