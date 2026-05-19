// AUDIO.md §7 — single translation unit that produces the miniaudio
// implementation. miniaudio is a single-header library (declarations
// + definitions in one .h); defining MINIAUDIO_IMPLEMENTATION before
// the include emits the function bodies once, here.
//
// Backend selection is default (miniaudio picks the platform's
// preferred backend at engine init: WASAPI on Windows, CoreAudio on
// macOS, ALSA/PulseAudio on Linux, OpenSL on Android, Web Audio on
// Emscripten). Backend symbols are resolved at runtime (dlopen /
// GetProcAddress) so no extra link-time deps are required — keeps
// the conanfile clean.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
