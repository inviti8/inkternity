// Headless render spike for ANIMATED_IMPORTS.md Phase 1: load a DragonBones sample
// rig through the SkeletalRig facade (embedded runtime + Skia adapter), advance a
// few frames, and draw a posed frame to a PNG — proving the whole pipeline renders
// without wiring the app UI. Mirrors tools/tfx_render_spike.cpp.
//
//   cmake --build build --target dragonbones_render_spike --config Release
//   build/Release/dragonbones_render_spike.exe [sampleDir] [baseName] [out.png]

#include "../src/Skeletal/SkeletalRig.hpp"

#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/core/SkColor.h>
#include <include/encode/SkPngEncoder.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    const std::string dir  = argc > 1 ? argv[1] : "deps/dragonbones/sample/mecha_1004d";
    const std::string base = argc > 2 ? argv[2] : "mecha_1004d";
    const std::string out  = argc > 3 ? argv[3] : "dragonbones_spike.png";

    const auto ske = readFile(dir + "/" + base + "_ske.json");
    const auto tex = readFile(dir + "/" + base + "_tex.json");
    const auto png = readFile(dir + "/" + base + "_tex.png");
    if (ske.empty() || tex.empty() || png.empty()) {
        std::cerr << "missing sample files under " << dir << "\n";
        return 2;
    }

    const int W = 1024, H = 1024;
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
    if (!surface) { std::cerr << "no surface\n"; return 4; }
    auto* c = surface->getCanvas();
    c->clear(SkColorSetARGB(255, 40, 44, 52));

    {   // rig scope — destroyed (atlas released) before shutdownRuntime()
        AI::SkeletalRig rig;
        if (!rig.load(ske, tex, png.data(), png.size())) {
            std::cerr << "rig load failed\n";
            return 3;
        }
        const auto anims = rig.animationNames();
        std::cerr << "loaded rig; animations=" << anims.size() << "\n";
        for (const auto& a : anims) std::cerr << "  - " << a << "\n";
        const std::string clip =
            std::find(anims.begin(), anims.end(), std::string("idle")) != anims.end()
                ? "idle" : (anims.empty() ? std::string() : anims[0]);
        if (!clip.empty()) rig.play(clip, 0);
        for (int i = 0; i < 30; ++i) rig.update(1.0f / 60.0f);   // ~0.5s in

        c->save();
        c->translate(W * 0.5f, H * 0.6f);   // rigs are authored around origin, feet near y=0
        rig.draw(c);
        c->restore();
    }
    AI::SkeletalRig::shutdownRuntime();   // flush the DragonBones pool while Skia is alive

    auto img = surface->makeImageSnapshot();
    SkPixmap pm;
    if (!img || !img->peekPixels(&pm)) { std::cerr << "peekPixels failed\n"; return 5; }
    SkFILEWStream fs(out.c_str());
    if (!SkPngEncoder::Encode(&fs, pm, {})) { std::cerr << "png encode failed\n"; return 6; }
    std::cerr << "wrote " << out << "\n";
    return 0;
}
