#pragma once
// Plain facade over the DragonBones runtime + Skia adapter (ANIMATED_IMPORTS.md §4).
// PIMPL: NO DragonBones or adapter types leak here, so the C++23 main target can
// include this while the adapter TU compiles isolated at C++17. The app talks to a
// rig only through: load → play → update → draw.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class SkCanvas;

namespace AI {

class SkeletalRig {
public:
    SkeletalRig();
    ~SkeletalRig();
    SkeletalRig(const SkeletalRig&) = delete;
    SkeletalRig& operator=(const SkeletalRig&) = delete;

    // Load a rig from in-memory data: DragonBones skeleton JSON, texture-atlas JSON,
    // and the decoded atlas PNG bytes. Returns false on any parse/decode failure.
    bool load(const std::string& skeletonJson,
              const std::string& atlasJson,
              const void* atlasPngBytes, std::size_t atlasPngLen);

    bool valid() const;
    std::vector<std::string> animationNames() const;

    // Play an animation. playTimes: 0 = loop forever, 1 = once, n = n times.
    void play(const std::string& name, int playTimes = 0);

    // Stop all animation (holds the current pose). Used to park an ON_TOUCH rig.
    void stop();

    // True while an animation is actively advancing (a loop, or a one-shot that
    // hasn't finished). False at rest or once a one-shot has completed.
    bool isPlaying() const;

    // Advance the animation clock by dtSeconds (recomputes bones + mesh deform).
    void update(float dtSeconds);

    // Draw the current pose. The caller sets the canvas matrix for placement/scale.
    void draw(SkCanvas* canvas) const;

    // Flush the DragonBones static object pool. Call ONCE at app shutdown, after all
    // rigs are destroyed but while Skia is still alive — the pooled objects would
    // otherwise be freed at static-teardown (after Skia), which crashes.
    static void shutdownRuntime();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace AI
