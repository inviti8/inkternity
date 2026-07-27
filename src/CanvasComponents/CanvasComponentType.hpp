#pragma once
#include <cstdint>

enum class CanvasComponentType : uint8_t {
    BRUSHSTROKE = 0,
    RECTANGLE,
    ELLIPSE,
    TEXTBOX,
    IMAGE,
    MYPAINTLAYER,
    WAYPOINT,
    PARTICLE,
    ARMATURE,  // PHASE9: re-editable posed-figure component (draws a baked raster)
    VECTORGROUP  // Consolidate Vectors: many brush strokes baked into one vector component (z-ordered sub-strokes, stays pure vector)
};
