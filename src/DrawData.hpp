#pragma once
#include "DrawCamera.hpp"
#include "ResourceManager.hpp"
#include "FontData.hpp"

using namespace Eigen;

class Toolbar;
class MainProgram;

struct DrawData {
    DrawCamera cam;
    ResourceManager* rMan;
    MainProgram* main;
    bool takingScreenshot = false;
    bool isSVGRender = false;
    bool clampDrawBetween = true;
    bool drawGrids = true;
    bool transparentBackground = false;
    bool skiaAA = false;
    // PHASE4 Part A: set by DrawingProgram::draw's parallax-bypass path.
    // Selected components live outside the draw cache and are rendered by
    // DrawingProgramSelection::draw_components; when the layer tree is
    // walked directly instead of through the cache, they must be skipped
    // here or they'd draw twice (in place + transformed preview).
    bool skipSelectedComponents = false;
    WorldScalar clampDrawMinimum;
    WorldScalar mipMapLevelOne;
    WorldScalar mipMapLevelTwo;
    void refresh_draw_optimizing_values();
};
