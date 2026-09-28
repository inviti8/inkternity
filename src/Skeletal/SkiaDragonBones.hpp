#pragma once
// Skia display adapter for the vendored DragonBonesCPP core (ANIMATED_IMPORTS.md
// §4-5). Mirrors the SFML reference adapter (deps/dragonbones/SFML/src), swapping
// SFML draw types for Skia SkVertices / SkImage. Compiled ISOLATED at C++17 in the
// dragonbones_skia lib (never included by the C++23 main target — the app sees only
// the plain SkeletalRig facade).
//
// A slot's display is EITHER geometry (SkiaDisplay) OR a child armature's proxy
// (SkiaArmatureProxy) — DragonBones rigs nest armatures. Both derive from SkiaDrawable
// so a parent proxy can hold and draw them polymorphically (mirrors SFML's sf::Drawable
// node base). Getting this right matters: mis-casting a child proxy to a geometry
// display corrupts the heap.

#include <dragonBones/DragonBonesHeaders.h>

#include <include/core/SkImage.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkVertices.h>
#include <include/core/SkPaint.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPoint.h>
#include <include/core/SkColor.h>

#include <cstdint>
#include <vector>

DRAGONBONES_NAMESPACE_BEGIN

// Common node base for anything a proxy can draw (geometry or a child armature).
class SkiaDrawable {
public:
    virtual ~SkiaDrawable() = default;
    SkMatrix matrix = SkMatrix::I();
    int      zOffset = 0;
    bool     visible = true;

    void setZOffset(int z) { zOffset = z; }
    int  getZOffset() const { return zOffset; }
    virtual void setVisible(bool v) { visible = v; }
    virtual void setColor(SkColor) {}

    // Matches SFMLNode::setMatrix — a DragonBones Matrix + pivot offset + scale.
    void setMatrix(const Matrix& m, float offX, float offY, float scaleX, float scaleY) {
        matrix = SkMatrix::MakeAll(m.a * scaleX, m.c * scaleY, offX,
                                   m.b * scaleX, m.d * scaleY, offY,
                                   0.f, 0.f, 1.f);
    }

    virtual void draw(SkCanvas* canvas) const = 0;

    // Expand [minX,minY]..[maxX,maxY] (rig-local space) to include this drawable's
    // geometry under `parent` * this node's own matrix. Used to size the on-canvas
    // clip/collider box to the actual (animated) pose, so a jump/reach isn't cropped.
    virtual void accumulateBounds(const SkMatrix& parent,
                                  float& minX, float& minY, float& maxX, float& maxY) const {}
};

// A slot's geometry (textured triangles), combining SFMLNode + SFMLDisplay.
class SkiaDisplay : public SkiaDrawable {
public:
    SkImage*              texture = nullptr;  // NON-owning; atlas lifetime is the rig's (facade)
    std::vector<SkPoint>  positions;    // slot-local vertex positions (mutated by skinning)
    std::vector<SkPoint>  texCoords;    // atlas pixel coordinates
    std::vector<SkColor>  colors;       // per-vertex modulate colour
    std::vector<uint16_t> indices;      // triangle list
    SkBlendMode           blend = SkBlendMode::kSrcOver;

    void setColor(SkColor c) override { for (auto& col : colors) col = c; }
    void draw(SkCanvas* canvas) const override;
    void accumulateBounds(const SkMatrix& parent,
                          float& minX, float& minY, float& maxX, float& maxY) const override;
};

class SkiaTextureData : public TextureData {
    BIND_CLASS_TYPE_B(SkiaTextureData);
public:
    SkImage* texture = nullptr;   // NON-owning ref into the atlas
    SkiaTextureData() { _onClear(); }
    ~SkiaTextureData() override { _onClear(); }
    void _onClear() override { texture = nullptr; TextureData::_onClear(); }
};

class SkiaTextureAtlasData : public TextureAtlasData {
    BIND_CLASS_TYPE_B(SkiaTextureAtlasData);
public:
    SkImage* renderTexture = nullptr;   // NON-owning; owned by SkeletalRig for the rig's lifetime
    SkiaTextureAtlasData() { _onClear(); }
    ~SkiaTextureAtlasData() override { _onClear(); }
    void _onClear() override { renderTexture = nullptr; TextureAtlasData::_onClear(); }
    TextureData* createTexture() const override;
};

class SkiaEventDispatcher : public IEventDispatcher {
    using Func = std::function<void(EventObject*)>;
public:
    void addDBEventListener(const std::string& type, const Func& l) override { _listeners[type].push_back(l); }
    void removeDBEventListener(const std::string&, const Func&) override {}
    bool hasDBEventListener(const std::string&) const override { return true; }
    void dispatchDBEvent(const std::string& type, EventObject* v) override {
        for (auto& l : _listeners[type]) l(v);
    }
private:
    std::unordered_map<std::string, std::vector<Func>> _listeners;
};

class SkiaSlot : public Slot {
    BIND_CLASS_TYPE_A(SkiaSlot);
public:
    void _updateVisible() override;
    void _updateBlendMode() override;
    void _updateColor() override;
protected:
    void _initDisplay(void* value, bool isRetain) override {}
    void _disposeDisplay(void* value, bool isRelease) override;
    void _onUpdateDisplay() override;
    void _addDisplay() override;
    void _replaceDisplay(void* value, bool isArmatureDisplay) override;
    void _removeDisplay() override;
    void _updateZOrder() override;
    void _updateFrame() override;
    void _updateMesh() override;
    void _updateTransform() override;
    void _identityTransform() override;
    void _onClear() override;
private:
    float         _textureScale = 1.f;
    SkiaDrawable* _renderDisplay = nullptr;   // SkiaDisplay OR a child SkiaArmatureProxy
};

class SkiaFactory;

class SkiaArmatureProxy : public SkiaDrawable, public IArmatureProxy {
    friend class SkiaFactory;
public:
    // IEventDispatcher
    bool hasDBEventListener(const std::string&) const override { return true; }
    void addDBEventListener(const std::string& type, const std::function<void(EventObject*)>& l) override { _dispatcher.addDBEventListener(type, l); }
    void removeDBEventListener(const std::string&, const std::function<void(EventObject*)>&) override {}
    void dispatchDBEvent(const std::string& type, EventObject* v) override { _dispatcher.dispatchDBEvent(type, v); }
    // IArmatureProxy
    void dbInit(Armature* armature) override { _armature = armature; }
    void dbClear() override { _armature = nullptr; _nodes.clear(); }
    void dbUpdate() override {}
    void dispose(bool) override { if (_armature) { _armature->dispose(); _armature = nullptr; } }
    Armature*  getArmature()  const override { return _armature; }
    Animation* getAnimation() const override { return _armature->getAnimation(); }
    // SkiaDrawable — draw this armature's slots (child armatures recurse through here)
    void draw(SkCanvas* canvas) const override;
    void accumulateBounds(const SkMatrix& parent,
                          float& minX, float& minY, float& maxX, float& maxY) const override;
    void setVisible(bool v) override { visible = v; for (auto n : _nodes) n->setVisible(v); }
    void setColor(SkColor c) override { for (auto n : _nodes) n->setColor(c); }
    // node list
    void addNode(SkiaDrawable* n) { _nodes.push_back(n); }
    void removeNode(SkiaDrawable* n);
    void sortNodes();
private:
    Armature*                  _armature = nullptr;
    SkiaEventDispatcher        _dispatcher;
    std::vector<SkiaDrawable*> _nodes;
};

class SkiaFactory : public BaseFactory {
public:
    SkiaFactory();
    ~SkiaFactory() override;

    // Parse from in-memory strings (no filesystem). `atlasImage` is the decoded PNG.
    DragonBonesData* loadDragonBonesData(const std::string& jsonContent, const std::string& name = "");
    TextureAtlasData* loadTextureAtlasData(const std::string& jsonContent, sk_sp<SkImage> atlasImage,
                                           const std::string& name = "", float scale = 1.f);
    SkiaArmatureProxy* buildArmatureDisplay(const std::string& armatureName,
                                            const std::string& dragonBonesName = "") const;
    void update(float dtSeconds) { _dragonBonesInstance->advanceTime(dtSeconds); }

protected:
    TextureAtlasData* _buildTextureAtlasData(TextureAtlasData* textureAtlasData, void* textureAtlas) const override;
    Armature* _buildArmature(const BuildArmaturePackage& dataPackage) const override;
    Slot* _buildSlot(const BuildArmaturePackage& dataPackage, const SlotData* slotData, Armature* armature) const override;

private:
    static DragonBones* _dragonBonesInstance;
    static std::unique_ptr<SkiaEventDispatcher> _soundDispatcher;
};

DRAGONBONES_NAMESPACE_END
