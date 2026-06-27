#include "VersionConstants.hpp"
#include <stdexcept>

namespace VersionConstants {

VersionNumber header_to_version_number(const std::string& header) {
    static std::unordered_map<std::string, VersionNumber> m;
    if(m.empty()) {
        m["INFPNT000001"] = VersionNumber(0, 0, 1);
        m["INFPNT000002"] = VersionNumber(0, 1, 0);
        m["INFPNT000003"] = VersionNumber(0, 2, 0);
        m["INFPNT000004"] = VersionNumber(0, 3, 0);
        m["INFPNT000005"] = VersionNumber(0, 4, 0);
        m["INFPNT000006"] = VersionNumber(0, 5, 0);
        m["INFPNT000007"] = VersionNumber(0, 6, 0);
        m["INFPNT000008"] = VersionNumber(0, 7, 0);
        m["INFPNT000009"] = VersionNumber(0, 8, 0);
        m["INFPNT000010"] = VersionNumber(0, 9, 0);
        m["INFPNT000011"] = VersionNumber(0, 10, 0);
        m["INFPNT000012"] = VersionNumber(0, 11, 0);
        m["INFPNT000013"] = VersionNumber(0, 12, 0); // PHASE4: layer DisplayData gains parallaxDepth + parallaxAnchor
        m["INFPNT000014"] = VersionNumber(0, 13, 0); // PHASE5: CanvasComponentType::PARTICLE (embedded .tfx packages)
        m["INFPNT000015"] = VersionNumber(0, 14, 0); // PHASE5.1: PARTICLE -> legacy .eff (libraryResourceId + effectName)
        m["INFPNT000016"] = VersionNumber(0, 15, 0); // PHASE5.1 polish: PARTICLE gains playMode (AUTO / on-touch)
        m["INFPNT000017"] = VersionNumber(0, 16, 0); // PHASE6: RECTANGLE gains polygonMode + points (editable polygons)
        m["INFPNT000018"] = VersionNumber(0, 17, 0); // PHASE6: ELLIPSE gains affineMode + center/tipA/tipB (shear/skew)
        m["INFPNT000019"] = VersionNumber(0, 18, 0); // PHASE7: RECTANGLE/ELLIPSE gain isMask + maskInvert (shape masks)
        m["INFPNT000020"] = VersionNumber(0, 19, 0); // PHASE8: polygon nodes gain bezier tangents (controlIn/controlOut/nodeType)
        m["INFPNT000021"] = VersionNumber(0, 20, 0); // PARALLAX-SCENES: parallax moves to group level — folder DisplayData gains parallaxRefScale; layer keeps parallaxDepth (anchor now group-level)
        m["INFPNT000022"] = VersionNumber(0, 21, 0); // PHASE9: CanvasComponentType::ARMATURE (re-editable posed-figure component; pose/camera/light + baked raster)
        m["INFPNT000023"] = VersionNumber(0, 22, 0); // PHASE9 M5.1a: ARMATURE gains height (uniform bone-length scale)
        m["INFPNT000024"] = VersionNumber(0, 23, 0); // PHASE9 M5.1b: ARMATURE gains per-material color overrides
    }
    auto it = m.find(header);
    if(it == m.end())
        throw std::runtime_error("[VersionConstants::header_to_version_number] Header " + header + " is invalid");
    return it->second;
}

}
