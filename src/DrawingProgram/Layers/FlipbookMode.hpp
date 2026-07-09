#pragma once
#include <cstdint>

// PHASE10 Feature B — a flip-book layer group treats its child layers as
// sequential animation frames (docs/design/PHASE10.md). A folder is a flip-book
// group when its DisplayData::flipbookFps > 0 (the enable flag + rate in one,
// mirroring how parallaxRefScale == 0 disables a parallax group).
//
// Playback is two orthogonal axes, deliberately matching the particle-FX modes
// (ParticlePlayMode) so the two systems read the same to an artist:
//   play style — how the frame sequence advances.
//   trigger    — what starts a playthrough (1:1 with ParticlePlayMode).
// The trigger only STARTS the state machine; the play style alone decides
// whether it ever ends (loop/ping-pong run forever; once rests on frame 1).
//
// Frame order defaults to top (first) -> bottom (last) in the layer panel;
// flipbookInvert plays bottom -> top instead.

enum class FlipbookPlayStyle : uint8_t {
    ONCE      = 0,  // one pass, then rest on frame 1
    LOOP      = 1,  // wrap to frame 1 forever
    PING_PONG = 2   // bounce forward/back forever
};

enum class FlipbookTriggerMode : uint8_t {
    AUTO     = 0,  // play when the group's region enters view (rising edge)
    ON_TOUCH = 1   // wait for a reader-mode tap on the group, then play
};
