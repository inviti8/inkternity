#include "SkiaDragonBones.hpp"

#include <include/core/SkShader.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkTileMode.h>

#include <algorithm>

DRAGONBONES_NAMESPACE_BEGIN

// ---------------------------------------------------------------- SkiaDisplay

void SkiaDisplay::draw(SkCanvas* canvas) const {
    if (!visible || !texture || positions.empty() || indices.empty())
        return;
    auto verts = SkVertices::MakeCopy(
        SkVertices::kTriangles_VertexMode,
        static_cast<int>(positions.size()),
        positions.data(),
        texCoords.data(),
        colors.empty() ? nullptr : colors.data(),
        static_cast<int>(indices.size()),
        indices.data());
    if (!verts) return;

    SkPaint paint;
    paint.setShader(texture->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                        SkSamplingOptions(SkFilterMode::kLinear)));
    canvas->save();
    canvas->concat(matrix);
    canvas->drawVertices(verts, SkBlendMode::kModulate, paint);   // vertex colours modulate atlas
    canvas->restore();
}

// ---------------------------------------------------- SkiaTextureAtlasData

TextureData* SkiaTextureAtlasData::createTexture() const {
    return BaseObject::borrowObject<SkiaTextureData>();
}

// Propagate the atlas image to every parsed sub-texture; swap region dims for rotated
// packs (mirrors SFMLTextureAtlasData::setRenderTexture). Non-owning pointers.
static void skia_set_render_texture(SkiaTextureAtlasData* self, SkImage* image) {
    self->renderTexture = image;
    if (!image) return;
    for (const auto& pair : self->textures) {
        auto td = static_cast<SkiaTextureData*>(pair.second);
        if (td->texture == nullptr) {
            Rectangle region;
            region.x = td->region.x;
            region.y = td->region.y;
            region.width  = td->rotated ? td->region.height : td->region.width;
            region.height = td->rotated ? td->region.width  : td->region.height;
            td->texture = image;
            td->region = region;
        }
    }
}

// ------------------------------------------------------------- SkiaArmatureProxy

void SkiaArmatureProxy::removeNode(SkiaDrawable* n) {
    auto it = std::find(_nodes.begin(), _nodes.end(), n);
    if (it != _nodes.end()) _nodes.erase(it);
}

void SkiaArmatureProxy::sortNodes() {
    std::sort(_nodes.begin(), _nodes.end(),
              [](SkiaDrawable* a, SkiaDrawable* b) { return a->getZOffset() < b->getZOffset(); });
}

void SkiaArmatureProxy::draw(SkCanvas* canvas) const {
    canvas->save();
    canvas->concat(matrix);                 // own transform (identity for root; set for child armatures)
    for (auto node : _nodes)
        if (node) node->draw(canvas);        // child armatures recurse through their own draw()
    canvas->restore();
}

void SkiaDisplay::accumulateBounds(const SkMatrix& parent,
                                   float& minX, float& minY, float& maxX, float& maxY) const {
    if (!visible || positions.empty()) return;
    // Bounds of the slot-local vertices, mapped by parent * this display's matrix
    // (the same transform draw() concats) → rig-local axis-aligned bounds.
    float lx = positions[0].fX, ly = positions[0].fY, hx = lx, hy = ly;
    for (const auto& p : positions) {
        lx = std::min(lx, p.fX); ly = std::min(ly, p.fY);
        hx = std::max(hx, p.fX); hy = std::max(hy, p.fY);
    }
    SkRect mapped;
    SkMatrix::Concat(parent, matrix).mapRect(&mapped, SkRect::MakeLTRB(lx, ly, hx, hy));
    minX = std::min(minX, mapped.fLeft);   minY = std::min(minY, mapped.fTop);
    maxX = std::max(maxX, mapped.fRight);  maxY = std::max(maxY, mapped.fBottom);
}

void SkiaArmatureProxy::accumulateBounds(const SkMatrix& parent,
                                         float& minX, float& minY, float& maxX, float& maxY) const {
    if (!visible) return;
    const SkMatrix m = SkMatrix::Concat(parent, matrix);
    for (auto node : _nodes)
        if (node) node->accumulateBounds(m, minX, minY, maxX, maxY);
}

// -------------------------------------------------------------------- SkiaSlot

void SkiaSlot::_updateVisible() {
    if (_renderDisplay) _renderDisplay->setVisible(_parent->getVisible());
}

static SkBlendMode map_blend(BlendMode m) {
    switch (m) {
        case BlendMode::Add:      return SkBlendMode::kPlus;
        case BlendMode::Multiply: return SkBlendMode::kModulate;
        case BlendMode::Screen:   return SkBlendMode::kScreen;
        default:                  return SkBlendMode::kSrcOver;
    }
}

void SkiaSlot::_updateBlendMode() {
    if (_childArmature) {
        for (const auto slot : _childArmature->getSlots()) {
            slot->_blendMode = _blendMode;
            slot->_updateBlendMode();
        }
    } else if (_renderDisplay) {
        static_cast<SkiaDisplay*>(_renderDisplay)->blend = map_blend(_blendMode);
    }
}

void SkiaSlot::_updateColor() {
    if (!_renderDisplay) return;
    const auto a = static_cast<U8CPU>(_colorTransform.alphaMultiplier * 255.f);
    const auto r = static_cast<U8CPU>(_colorTransform.redMultiplier   * 255.f);
    const auto g = static_cast<U8CPU>(_colorTransform.greenMultiplier * 255.f);
    const auto b = static_cast<U8CPU>(_colorTransform.blueMultiplier  * 255.f);
    _renderDisplay->setColor(SkColorSetARGB(a, r, g, b));   // virtual: geometry sets verts, proxy recurses
}

void SkiaSlot::_disposeDisplay(void* value, bool isRelease) {
    if (!isRelease && value) delete static_cast<SkiaDisplay*>(value);
}

void SkiaSlot::_onUpdateDisplay() {
    _renderDisplay = static_cast<SkiaDrawable*>(_display != nullptr ? _display : _rawDisplay);
    _renderDisplay->setZOffset(_slotData->zOrder);
}

void SkiaSlot::_addDisplay() {
    static_cast<SkiaArmatureProxy*>(_armature->getDisplay())->addNode(_renderDisplay);
}

void SkiaSlot::_replaceDisplay(void* value, bool) {
    auto prev = static_cast<SkiaDrawable*>(value);
    auto arm = static_cast<SkiaArmatureProxy*>(_armature->getDisplay());
    _renderDisplay->setZOffset(prev->getZOffset());
    arm->removeNode(prev);
    arm->addNode(_renderDisplay);
    arm->sortNodes();
    _textureScale = 1.f;
}

void SkiaSlot::_removeDisplay() {
    static_cast<SkiaArmatureProxy*>(_armature->getDisplay())->removeNode(_renderDisplay);
}

void SkiaSlot::_updateZOrder() {
    _renderDisplay->setZOffset(_slotData->zOrder);
    static_cast<SkiaArmatureProxy*>(_armature->getDisplay())->sortNodes();
}

void SkiaSlot::_updateFrame() {
    const auto currentVerticesData =
        (_deformVertices != nullptr && _display == _meshDisplay) ? _deformVertices->verticesData : nullptr;
    auto textureData = static_cast<SkiaTextureData*>(_textureData);

    if (!_childArmature && _displayIndex >= 0 && _display != nullptr &&
        textureData != nullptr && textureData->texture != nullptr) {
        auto display = static_cast<SkiaDisplay*>(_renderDisplay);
        display->texture = textureData->texture;
        const auto& region = textureData->region;

        if (currentVerticesData != nullptr) { // Mesh
            const auto data = currentVerticesData->data;
            const auto intArray = data->intArray;
            const auto floatArray = data->floatArray;
            const unsigned vertexCount   = (unsigned)intArray[currentVerticesData->offset + (unsigned)BinaryOffset::MeshVertexCount];
            const unsigned triangleCount = (unsigned)intArray[currentVerticesData->offset + (unsigned)BinaryOffset::MeshTriangleCount];
            int vertexOffset = intArray[currentVerticesData->offset + (unsigned)BinaryOffset::MeshFloatOffset];
            if (vertexOffset < 0) vertexOffset += 65536;
            const unsigned uvOffset = vertexOffset + vertexCount * 2;

            display->positions.resize(vertexCount);
            display->texCoords.resize(vertexCount);
            display->colors.assign(vertexCount, SK_ColorWHITE);
            display->indices.resize(triangleCount * 3);

            for (unsigned i = 0, l = vertexCount * 2; i < l; i += 2) {
                const auto iH = i / 2;
                const float x = floatArray[vertexOffset + i];
                const float y = floatArray[vertexOffset + i + 1];
                const float u = floatArray[uvOffset + i];
                const float v = floatArray[uvOffset + i + 1];
                display->positions[iH] = { x, y };
                if (textureData->rotated)
                    display->texCoords[iH] = { region.x + (1.f - v) * region.width, region.y + u * region.height };
                else
                    display->texCoords[iH] = { region.x + u * region.width, region.y + v * region.height };
            }
            for (unsigned i = 0; i < triangleCount * 3; ++i)
                display->indices[i] = (uint16_t)intArray[currentVerticesData->offset + (unsigned)BinaryOffset::MeshVertexIndices + i];

            _textureScale = 1.f;
            if (currentVerticesData->weight != nullptr) _identityTransform();  // skinned
        } else { // Normal quad
            const auto scale = textureData->parent->scale * _armature->_armatureData->scale;
            _textureScale = scale;
            const float w = std::abs(region.width);
            const float h = std::abs(region.height);
            display->positions = { {0.f, 0.f}, {0.f, h}, {w, 0.f}, {w, h} };
            display->texCoords = {
                { region.x,                region.y },
                { region.x,                region.y + region.height },
                { region.x + region.width, region.y },
                { region.x + region.width, region.y + region.height },
            };
            display->colors.assign(4, SK_ColorWHITE);
            display->indices = { 0, 1, 2, 2, 1, 3 };
        }

        _visibleDirty = true;
        _blendModeDirty = true;
        _colorDirty = true;
        return;
    }

    // No renderable geometry (empty slot). Child-armature slots render themselves.
    if (_renderDisplay && !_childArmature) _renderDisplay->setVisible(false);
}

void SkiaSlot::_updateMesh() {
    const auto scale = _armature->_armatureData->scale;
    const auto& deformVertices = _deformVertices->vertices;
    const auto& bones = _deformVertices->bones;
    const auto verticesData = _deformVertices->verticesData;
    const auto weightData = verticesData->weight;
    const auto hasFFD = !deformVertices.empty();
    auto display = static_cast<SkiaDisplay*>(_renderDisplay);

    if (weightData != nullptr) {
        const auto data = verticesData->data;
        const auto intArray = data->intArray;
        const auto floatArray = data->floatArray;
        const auto vertexCount = (std::size_t)intArray[verticesData->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int weightFloatOffset = intArray[weightData->offset + (unsigned)BinaryOffset::WeigthFloatOffset];
        if (weightFloatOffset < 0) weightFloatOffset += 65536;

        for (std::size_t i = 0, iB = weightData->offset + (unsigned)BinaryOffset::WeigthBoneIndices + bones.size(),
                         iV = (std::size_t)weightFloatOffset;
             i < vertexCount; ++i) {
            const auto boneCount = (std::size_t)intArray[iB++];
            float xG = 0.f, yG = 0.f;
            for (std::size_t j = 0; j < boneCount; ++j) {
                const auto boneIndex = (unsigned)intArray[iB++];
                const auto bone = bones[boneIndex];
                if (bone != nullptr) {
                    const auto& matrix = bone->globalTransformMatrix;
                    const auto weight = floatArray[iV++];
                    const auto xL = floatArray[iV++] * scale;
                    const auto yL = floatArray[iV++] * scale;
                    xG += (matrix.a * xL + matrix.c * yL + matrix.tx) * weight;
                    yG += (matrix.b * xL + matrix.d * yL + matrix.ty) * weight;
                }
            }
            if (i < display->positions.size()) display->positions[i] = { xG, yG };
        }
    } else if (hasFFD) {
        const auto data = verticesData->data;
        const auto intArray = data->intArray;
        const auto floatArray = data->floatArray;
        const auto vertexCount = (std::size_t)intArray[verticesData->offset + (unsigned)BinaryOffset::MeshVertexCount];
        int vertexOffset = intArray[verticesData->offset + (unsigned)BinaryOffset::MeshFloatOffset];
        if (vertexOffset < 0) vertexOffset += 65536;
        for (std::size_t i = 0, l = vertexCount * 2; i < l; i += 2) {
            const auto iH = i / 2;
            const float xG = floatArray[vertexOffset + i] * scale + deformVertices[i];
            const float yG = floatArray[vertexOffset + i + 1] * scale + deformVertices[i + 1];
            if (iH < display->positions.size()) display->positions[iH] = { xG, yG };
        }
    }
}

void SkiaSlot::_identityTransform() {
    Matrix identity;
    _renderDisplay->setMatrix(identity, 0.f, 0.f, _textureScale, _textureScale);
}

void SkiaSlot::_updateTransform() {
    float px = globalTransformMatrix.tx;
    float py = globalTransformMatrix.ty;
    if (_renderDisplay == static_cast<SkiaDrawable*>(_rawDisplay) ||
        _renderDisplay == static_cast<SkiaDrawable*>(_meshDisplay)) {
        px -= globalTransformMatrix.a * _pivotX + globalTransformMatrix.c * _pivotY;
        py -= globalTransformMatrix.b * _pivotX + globalTransformMatrix.d * _pivotY;
    } else {
        px -= globalTransformMatrix.a - globalTransformMatrix.c;
        py -= globalTransformMatrix.b - globalTransformMatrix.d;
    }
    _renderDisplay->setMatrix(globalTransformMatrix, px, py, _textureScale, _textureScale);
}

void SkiaSlot::_onClear() {
    Slot::_onClear();
    _textureScale = 1.f;
    _renderDisplay = nullptr;
}

// ----------------------------------------------------------------- SkiaFactory

DragonBones* SkiaFactory::_dragonBonesInstance = nullptr;
std::unique_ptr<SkiaEventDispatcher> SkiaFactory::_soundDispatcher;

SkiaFactory::SkiaFactory() {
    if (_dragonBonesInstance == nullptr) {
        _soundDispatcher = std::make_unique<SkiaEventDispatcher>();
        _dragonBonesInstance = new DragonBones(_soundDispatcher.get());
    }
    _dragonBones = _dragonBonesInstance;
}

SkiaFactory::~SkiaFactory() { clear(); }

DragonBonesData* SkiaFactory::loadDragonBonesData(const std::string& jsonContent, const std::string& name) {
    if (!name.empty()) {
        const auto existed = getDragonBonesData(name);
        if (existed) return existed;
    }
    if (jsonContent.empty()) return nullptr;
    return parseDragonBonesData(jsonContent.c_str(), name, 1.f);
}

TextureAtlasData* SkiaFactory::loadTextureAtlasData(const std::string& jsonContent, sk_sp<SkImage> atlasImage,
                                                    const std::string& name, float scale) {
    if (jsonContent.empty()) return nullptr;
    return parseTextureAtlasData(jsonContent.c_str(), (void*)atlasImage.get(), name, scale);
}

SkiaArmatureProxy* SkiaFactory::buildArmatureDisplay(const std::string& armatureName,
                                                     const std::string& dragonBonesName) const {
    const auto armature = buildArmature(armatureName, dragonBonesName, "", "");
    if (armature != nullptr) {
        // NOTE: deliberately NOT added to the shared WorldClock — each rig is advanced
        // directly (SkeletalRig::update → armature->advanceTime).
        return static_cast<SkiaArmatureProxy*>(armature->getDisplay());
    }
    return nullptr;
}

TextureAtlasData* SkiaFactory::_buildTextureAtlasData(TextureAtlasData* textureAtlasData, void* textureAtlas) const {
    auto data = static_cast<SkiaTextureAtlasData*>(textureAtlasData);
    if (data != nullptr) {
        skia_set_render_texture(data, static_cast<SkImage*>(textureAtlas));
    } else {
        data = BaseObject::borrowObject<SkiaTextureAtlasData>();
    }
    return data;
}

Armature* SkiaFactory::_buildArmature(const BuildArmaturePackage& dataPackage) const {
    const auto armature = BaseObject::borrowObject<Armature>();
    const auto proxy = new SkiaArmatureProxy();
    // Pass the SkiaDrawable subobject as the armature's display, so getDisplay()
    // (void*) round-trips to a valid SkiaDrawable* for parent-slot node handling.
    armature->init(dataPackage.armature, proxy, static_cast<SkiaDrawable*>(proxy), _dragonBones);
    return armature;
}

Slot* SkiaFactory::_buildSlot(const BuildArmaturePackage&, const SlotData* slotData, Armature* armature) const {
    auto slot = BaseObject::borrowObject<SkiaSlot>();
    auto display = new SkiaDisplay();
    slot->init(slotData, armature, display, display);
    return slot;
}

DRAGONBONES_NAMESPACE_END
