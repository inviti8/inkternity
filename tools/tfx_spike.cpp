// M0 runtime spike / load harness for PHASE5 particle systems
// (docs/design/PHASE5.md).
//
// Proves the vendored TimelineFX runtime (deps/timelinefx, pinned commit
// a5f323d826fa0d6e7ccb481961c518c56aa59285) compiles, links, initialises,
// and — given a .tfx effect package path as argv[1] — validates and loads
// it, reporting shape count, error status, and effect names. No rendering
// yet; the drawAtlas path lands in the real ParticleCanvasComponent.
//
// Build explicitly (EXCLUDE_FROM_ALL):
//   cmake --build build --config Release --target tfx_spike
// Optionally point it at a package:
//   build/Release/tfx_spike.exe build/tfx_test/effects.tfx
#include <cstdio>
#include "timelinefx.h"

static int g_shapes_loaded = 0;

// In the real renderer this decodes raw_image_data into the Skia particle
// atlas and stashes the atlas handle on image_data->ptr. For the load test
// we only count shapes and stamp a non-null ptr so the library doesn't flag
// tfxErrorCode_library_loaded_without_shape_loader.
static void count_shape_loader(const char* filename, tfx_image_data_t* image_data,
                               void* raw_image_data, int image_size, void* user_data) {
    (void)filename; (void)raw_image_data; (void)image_size; (void)user_data;
    ++g_shapes_loaded;
    image_data->ptr = image_data; // dummy non-null
}

// Used by BuildLibraryGPUShapeData in a real GPU pipeline; harmless no-op here.
static void noop_uv_lookup(void* ptr, tfx_gpu_image_data_t* image_data, int offset) {
    (void)ptr; (void)image_data; (void)offset;
}

int main(int argc, char** argv) {
    std::printf("[tfx_spike] TimelineFX vendored runtime, version=%s\n", TFX_VERSION);

    tfx_InitialiseTimelineFX(1, 64ull * 1024 * 1024);
    std::printf("[tfx_spike] tfx_InitialiseTimelineFX OK\n");

    if (argc > 1) {
        const char* path = argv[1];
        std::printf("[tfx_spike] package: %s\n", path);

        int vrc = tfx_ValidateEffectPackage(path);
        std::printf("[tfx_spike] tfx_ValidateEffectPackage -> %d (0 = valid)\n", vrc);
        std::printf("[tfx_spike] tfx_GetShapeCountInLibrary -> %d\n",
                    tfx_GetShapeCountInLibrary(path));

        if (vrc == 0) {
            tfx_library lib = tfx_LoadEffectLibrary(path, count_shape_loader,
                                                    noop_uv_lookup, nullptr);
            tfxErrorFlags err = lib ? tfx_GetLibraryErrorStatus(lib) : 0xFFFFFFFFu;
            std::printf("[tfx_spike] tfx_LoadEffectLibrary -> handle=%p "
                        "error_status=0x%X shapes_loaded=%d\n",
                        (void*)lib, (unsigned)err, g_shapes_loaded);
            if (lib && err == 0) {
                std::printf("[tfx_spike] effect names in library:\n");
                ListEffectNames(lib);
            }
        }
    }

    tfx_EndTimelineFX();
    std::printf("[tfx_spike] tfx_EndTimelineFX OK\n");
    return 0;
}
