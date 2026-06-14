// M0 render proof / diagnostic harness for PHASE5 particle systems
// (docs/design/PHASE5.md).
//
// Loads a .tfx effect *library*, and for one effect (or every effect, in
// --all mode) runs TimelineFX's CPU simulation for N frames, then renders the
// resulting instance buffer with a faithful SkRuntimeEffect port of
// timelinefx.frag. Writes a PNG per effect and prints a per-effect tally of
// how many instances drew vs. were skipped (and why) — the sweep we use to see
// which effects in a library render, which are dim, and which draw nothing.
//
// Build (EXCLUDE_FROM_ALL, conan builds only):
//   cmake --build build --config Release --target tfx_render_spike
// Run (single effect):
//   build/Release/tfx_render_spike.exe library.tfx out.png "Explosion" 30 0 768
// Run (sweep every effect into a directory):
//   build/Release/tfx_render_spike.exe library.tfx out_dir __all__ 40 0 768
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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
size_t g_props_count = 0;              // bound for prop_idx (avoids OOB read)

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

struct EffectSummary {
    std::string name;
    int   instances = 0;
    int   particles = 0;
    int   drawn     = 0;
    int   sizeZero  = 0;   // skipped: zero sprite scale
    int   imgOOR    = 0;   // skipped: img_idx >= loaded shape count
    int   shapeNull = 0;   // skipped: decoded shape was null
    int   rampNull  = 0;   // skipped: no color ramp
    uint32_t maxImgIdx = 0;
    float extent    = 0.f;
    float scale     = 0.f;
    bool  added     = false;
    bool  wrotePng  = false;
};

std::string sanitize(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) ? c : '_');
    return o;
}

EffectSummary render_effect(tfx_library lib, tfx_effect_manager pm,
                            const sk_sp<SkRuntimeEffect>& effect,
                            tfx_gpu_particle_properties_t* props,
                            const std::vector<sk_sp<SkImage>>& ramps,
                            const std::string& name, int frames,
                            float scale_arg, int canvas_px,
                            const std::string& out_png) {
    EffectSummary sum;
    sum.name = name;

    tfx_ClearEffectManager(pm, true, true);
    tfx_effect_template tmpl = tfx_CreateEffectTemplate(lib, name.c_str());
    if (!tmpl) return sum;
    tfxEffectID id;
    if (!tfx_AddEffectTemplateToEffectManager(pm, tmpl, &id)) return sum;
    sum.added = true;
    tfx_SetEffectPosition(pm, id, 0.f, 0.f, 0.f);
    for (int f = 0; f < frames; ++f) tfx_UpdateEffectManager(pm, 1000.0 / 60.0);

    tfx_instance_t* inst = tfx_GetInstanceBuffer(pm);
    int count = tfx_GetInstanceCount(pm);
    sum.instances = count;
    sum.particles = (int)tfx_ParticleCount(pm);

    // Robust auto-fit: 90th-percentile of (|pos| + half sprite) so a few
    // far-flung particles don't shrink the whole frame to a dot.
    std::vector<float> radii;
    radii.reserve(count);
    for (int k = 0; k < count; ++k) {
        float s[2];
        tfx_GetSpriteScale(const_cast<tfx_instance_t*>(&inst[k]), s);
        radii.push_back(std::max(std::fabs(inst[k].position.x),
                                 std::fabs(inst[k].position.y)) + 0.5f * std::max(s[0], s[1]));
    }
    float extent = 1.f;
    if (!radii.empty()) {
        std::sort(radii.begin(), radii.end());
        extent = std::max(radii[(size_t)(radii.size() * 0.9f)], 1e-4f);
    }
    sum.extent = extent;
    float fit = (scale_arg > 0.f) ? scale_arg : (canvas_px * 0.42f) / extent;
    sum.scale = fit;

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(canvas_px, canvas_px));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorBLACK);
    const float cx = canvas_px * 0.5f, cy = canvas_px * 0.5f;
    const float kIntensityMax = 128.0f / 32767.0f;
    SkSamplingOptions linear(SkFilterMode::kLinear);

    for (int k = 0; k < count; ++k) {
        const tfx_instance_t& in = inst[k];
        float sz[2];
        tfx_GetSpriteScale(const_cast<tfx_instance_t*>(&in), sz);
        float w = sz[0] * fit, h = sz[1] * fit;
        uint32_t img_idx = in.indexes & 0x1FFFu;
        sum.maxImgIdx = std::max(sum.maxImgIdx, img_idx);

        if (w <= 0.f || h <= 0.f) { sum.sizeZero++; continue; }
        if (img_idx >= g_shapes.size()) { sum.imgOOR++; continue; }
        const sk_sp<SkImage>& shape = g_shapes[img_idx];
        if (!shape) { sum.shapeNull++; continue; }

        uint32_t prop_idx = (in.indexes >> 16) & 0xFFFFu;
        if (g_props_count && prop_idx >= g_props_count) { sum.imgOOR++; continue; }
        const tfx_gpu_particle_properties_t& pr = props[prop_idx];
        int ramp_array = (pr.color_ramp_indexes >> 8) & 0xFF;
        float ramp_y = static_cast<float>(pr.color_ramp_indexes & 0xFF);
        const sk_sp<SkImage>& ramp =
            (ramp_array < static_cast<int>(ramps.size())) ? ramps[ramp_array]
                                                          : (ramps.empty() ? nullptr : ramps[0]);
        if (!ramp) { sum.rampNull++; continue; }

        uint32_t lo = static_cast<uint32_t>(in.quaternion & 0xFFFFFFFFull);
        uint32_t hi = static_cast<uint32_t>(in.quaternion >> 32);
        float qx = snorm16(lo), qy = snorm16(lo >> 16), qz = snorm16(hi), qw = snorm16(hi >> 16);
        float ql = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
        if (ql > 1e-6f) { qx/=ql; qy/=ql; qz/=ql; qw/=ql; }
        float c00 = 1.f - 2.f*(qy*qy + qz*qz);
        float c01 = 2.f*(qx*qy + qz*qw);
        float angle_deg = std::atan2(c01, c00) * 180.0f / 3.14159265f;

        float intensity = in.intensity_gradient_map.x * kIntensityMax;
        float tex_influence = in.intensity_gradient_map.y * kIntensityMax;
        float sharpA = in.curved_alpha_life.x / 255.0f;
        float dissolveB = in.curved_alpha_life.y / 255.0f;
        float hx = pr.image_handle.x, hy = pr.image_handle.y;

        SkRuntimeShaderBuilder b(effect);
        b.child("uShape") = shape->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, linear);
        b.child("uRamp")  = ramp->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                             SkSamplingOptions(SkFilterMode::kNearest));
        b.uniform("uQuad")        = SkV2{w, h};
        b.uniform("uShapeSize")   = SkV2{(float)shape->width(), (float)shape->height()};
        b.uniform("uRampY")       = ramp_y;
        b.uniform("uIntensity")   = intensity;
        b.uniform("uTexInfluence")= tex_influence;
        b.uniform("uSharpA")      = sharpA;
        b.uniform("uDissolveB")   = dissolveB;
        b.uniform("uHeat") = SkV3{pr.heat_response_boost, pr.heat_response_sharpness,
                                  pr.heat_response_curve};

        SkPaint paint;
        paint.setShader(b.makeShader());
        paint.setBlendMode(SkBlendMode::kPlus);  // additive (diagnostic: bright on black)

        SkMatrix m = SkMatrix::Translate(cx + in.position.x * fit, cy + in.position.y * fit);
        m.preRotate(angle_deg);
        m.preTranslate(-hx * w, -hy * h);
        canvas->save();
        canvas->concat(m);
        canvas->drawRect(SkRect::MakeWH(w, h), paint);
        canvas->restore();
        sum.drawn++;
    }

    sum.wrotePng = write_png(surface->makeImageSnapshot(), out_png);
    return sum;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: tfx_render_spike <library.tfx> <out.png|out_dir> "
                    "[effect_name|__all__] [frames] [scale] [canvas_px]\n");
        return 2;
    }
    const char* path = argv[1];
    const std::string out_arg = argv[2];
    const std::string effect_name = argc > 3 ? argv[3] : "__all__";
    const int frames = argc > 4 ? std::atoi(argv[4]) : 40;
    const float scale = argc > 5 ? static_cast<float>(std::atof(argv[5])) : 0.0f; // 0 = auto-fit
    const int canvas_px = argc > 6 ? std::atoi(argv[6]) : 768;
    const bool sweep_all = (effect_name == "__all__");

    tfx_InitialiseTimelineFX(1, 128ull * 1024 * 1024);
    // some_data_not_loaded (0x10) just means newer alpha-editor property keys
    // were skipped — tolerate it, same as the app's loader does.
    const tfxErrorFlags kNonFatal = tfxErrorCode_some_data_not_loaded |
                                    tfxErrorCode_library_loaded_without_shape_loader;
    tfxErrorFlags vflags = tfx_ValidateEffectPackage(path);
    if (vflags & ~kNonFatal) {
        std::printf("[render] invalid package 0x%X: %s\n", vflags, path);
        return 1;
    }
    tfx_library lib = tfx_LoadEffectLibrary(path, skia_shape_loader, uv_lookup, nullptr);
    tfxErrorFlags lflags = lib ? tfx_GetLibraryErrorStatus(lib) : 0xFFFFFFFFu;
    if (!lib || (lflags & ~kNonFatal)) {
        std::printf("[render] library load error 0x%X\n", lflags);
        return 1;
    }
    if (lflags) std::printf("[render] loaded with skipped fields 0x%X\n", lflags);
    std::printf("[render] loaded %zu shapes\n", g_shapes.size());

    std::vector<std::string> names;
    for (tfx_effect_descriptor e : lib->effects) {
        const char* nm = tfx_GetEffectName(e);
        if (nm) names.push_back(nm);
    }
    std::printf("[render] %zu effects in library\n", names.size());

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
    std::printf("[render] %zu color-ramp bitmaps\n", ramps.size());
    if (!ramps.empty() && ramps[0])
        std::printf("[render] ramp0 -> _ramp0.png (%s)\n",
                    write_png(ramps[0], "_ramp0.png") ? "ok" : "fail");

    auto* props = static_cast<tfx_gpu_particle_properties_t*>(tfx_ParticlePropertiesBuffer(lib));
    g_props_count = tfx_ParticlePropertiesBufferSizeInBytes(lib) / sizeof(tfx_gpu_particle_properties_t);
    std::printf("[render] %zu particle-property entries\n", g_props_count);

    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(kParticleSkSL));
    if (!effect) { std::printf("[render] SkSL error: %s\n", err.c_str()); return 1; }

    tfx_effect_manager pm = tfx_CreateEffectManager(
        tfx_CreateEffectManagerInfo(tfxEffectManagerSetup_none));

    std::vector<std::string> targets;
    std::string out_dir;
    if (sweep_all) {
        out_dir = out_arg;
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        targets = names;
    } else {
        targets.push_back(effect_name);
    }

    std::vector<EffectSummary> rows;
    for (const std::string& nm : targets) {
        std::string out_png = sweep_all ? (out_dir + "/" + sanitize(nm) + ".png") : out_arg;
        rows.push_back(render_effect(lib, pm, effect, props, ramps, nm, frames, scale, canvas_px, out_png));
    }

    // Summary table.
    std::printf("\n%-26s %7s %7s %7s  %6s %6s %6s %6s  %7s %7s\n",
                "effect", "inst", "drawn", "maxImg", "size0", "imgOOR", "shpNul", "rmpNul",
                "extent", "scale");
    std::printf("%s\n", std::string(110, '-').c_str());
    for (const auto& r : rows) {
        if (!r.added) { std::printf("%-26s   (could not add effect)\n", r.name.c_str()); continue; }
        std::printf("%-26s %7d %7d %7u  %6d %6d %6d %6d  %7.2f %7.2f%s\n",
                    r.name.substr(0, 26).c_str(), r.instances, r.drawn, r.maxImgIdx,
                    r.sizeZero, r.imgOOR, r.shapeNull, r.rampNull, r.extent, r.scale,
                    r.wrotePng ? "" : "  [PNG FAIL]");
    }

    tfx_FreeEffectManager(pm);
    tfx_EndTimelineFX();
    return 0;
}
