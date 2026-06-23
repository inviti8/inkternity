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
    // PARALLAX-SCENES: a folder that is a parallax group sets this for its
    // subtree (DrawingProgramLayerListItem::draw); descendant layers read it
    // to derive their per-depth camera (§3 of PARALLAX-SCENES.md). A nested
    // group overwrites it for its own subtree (nearest ancestor wins).
    // Inactive by default → a layer with depth renders flat unless it's
    // inside an active group.
    struct ParallaxGroup {
        bool active = false;
        WorldScalar anchorX{0};
        WorldScalar anchorY{0};
        WorldScalar refScale{1};
    } parallaxGroup;
    WorldScalar clampDrawMinimum;
    WorldScalar mipMapLevelOne;
    WorldScalar mipMapLevelTwo;
    void refresh_draw_optimizing_values();
};
