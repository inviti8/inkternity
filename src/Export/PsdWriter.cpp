#include "PsdWriter.hpp"

#include <algorithm>
#include <cstring>

namespace PsdExport {

namespace {

void put16(std::string& o, uint16_t v) {
    o.push_back(static_cast<char>((v >> 8) & 0xFF));
    o.push_back(static_cast<char>(v & 0xFF));
}
void put32(std::string& o, uint32_t v) {
    o.push_back(static_cast<char>((v >> 24) & 0xFF));
    o.push_back(static_cast<char>((v >> 16) & 0xFF));
    o.push_back(static_cast<char>((v >> 8) & 0xFF));
    o.push_back(static_cast<char>(v & 0xFF));
}
void put32s(std::string& o, int32_t v) { put32(o, static_cast<uint32_t>(v)); }

// Legacy Pascal layer name, padded so the whole string is a multiple of 4 bytes.
std::string pascal_padded(const std::string& name) {
    std::string b = name.substr(0, 255);
    std::string s;
    s.push_back(static_cast<char>(b.size()));
    s += b;
    while (s.size() % 4 != 0) s.push_back('\0');
    return s;
}

// One channel plane (ch: 0=R,1=G,2=B,3=A) of a w*h straight-RGBA buffer.
void append_plane(std::string& o, const std::vector<uint8_t>& rgba, int32_t w, int32_t h, int ch) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    o.reserve(o.size() + n);
    for (size_t i = 0; i < n; ++i)
        o.push_back(static_cast<char>(rgba[i * 4 + ch]));
}

// Straight-alpha "source over dest" of one 8-bit pixel component.
inline uint8_t over8(uint8_t sc, uint8_t sa, uint8_t dc, uint8_t da, uint8_t outA) {
    if (outA == 0) return 0;
    // out = (sc*sa + dc*da*(1-sa)) / outA, all in 0..255 space
    const int num = sc * sa + (dc * da * (255 - sa)) / 255;
    return static_cast<uint8_t>(std::min(255, num / outA));
}

// Composite layers (bottom-to-top) into a doc-sized straight-RGBA buffer.
std::vector<uint8_t> make_composite(int32_t docW, int32_t docH, const std::vector<Layer>& layers) {
    std::vector<uint8_t> dst(static_cast<size_t>(docW) * docH * 4, 0);
    for (const auto& L : layers) {
        if (L.kind != LayerKind::Image || !L.visible || L.rgba.empty()) continue;
        for (int32_t ly = 0; ly < L.height; ++ly) {
            const int32_t dy = L.y + ly;
            if (dy < 0 || dy >= docH) continue;
            for (int32_t lx = 0; lx < L.width; ++lx) {
                const int32_t dx = L.x + lx;
                if (dx < 0 || dx >= docW) continue;
                const size_t si = (static_cast<size_t>(ly) * L.width + lx) * 4;
                uint8_t sr = L.rgba[si], sg = L.rgba[si + 1], sb = L.rgba[si + 2];
                uint8_t sa = static_cast<uint8_t>(L.rgba[si + 3] * L.opacity / 255);
                if (sa == 0) continue;
                const size_t di = (static_cast<size_t>(dy) * docW + dx) * 4;
                uint8_t dr = dst[di], dg = dst[di + 1], db = dst[di + 2], da = dst[di + 3];
                const uint8_t oa = static_cast<uint8_t>(sa + da * (255 - sa) / 255);
                dst[di]     = over8(sr, sa, dr, da, oa);
                dst[di + 1] = over8(sg, sa, dg, da, oa);
                dst[di + 2] = over8(sb, sa, db, da, oa);
                dst[di + 3] = oa;
            }
        }
    }
    return dst;
}

}  // namespace

std::string build_psd(int32_t docW, int32_t docH, const std::vector<Layer>& layers) {
    if (docW <= 0 || docH <= 0) return {};

    std::string f;

    // ---- File header ----
    f += "8BPS";
    put16(f, 1);                 // version 1
    f.append(6, '\0');           // reserved
    put16(f, 4);                 // channels (R,G,B,A)
    put32(f, static_cast<uint32_t>(docH));
    put32(f, static_cast<uint32_t>(docW));
    put16(f, 8);                 // depth
    put16(f, 3);                 // color mode = RGB

    put32(f, 0);                 // Color Mode Data (none)
    put32(f, 0);                 // Image Resources (none)

    // ---- Layer Info ----
    std::string li;
    put16(li, static_cast<uint16_t>(static_cast<int16_t>(layers.size())));  // layer count (>=0)

    static constexpr int16_t CH_ID[4] = {0, 1, 2, -1};   // R,G,B,A(=-1)
    static constexpr int     CH_SRC[4] = {0, 1, 2, 3};

    // A 4-char PSD blend key, space-padded/truncated defensively.
    auto blend4 = [](const std::string& b) {
        std::string s = b;
        s.resize(4, ' ');
        return s;
    };

    for (const auto& L : layers) {
        const bool isImage = (L.kind == LayerKind::Image);
        const int32_t w = isImage ? L.width  : 0;   // marker layers are 0x0
        const int32_t h = isImage ? L.height : 0;
        put32s(li, isImage ? L.y : 0);             // top
        put32s(li, isImage ? L.x : 0);             // left
        put32s(li, isImage ? (L.y + L.height) : 0);// bottom
        put32s(li, isImage ? (L.x + L.width) : 0); // right
        put16(li, 4);                    // channel count
        const uint32_t chBytes = 2u + static_cast<uint32_t>(w) * h;  // 2 (compression) + raw
        for (int c = 0; c < 4; ++c) {
            put16(li, static_cast<uint16_t>(CH_ID[c]));
            put32(li, chBytes);
        }
        li += "8BIM";
        li += blend4(isImage ? L.blend : std::string("norm"));  // group markers: normal
        li.push_back(static_cast<char>(L.opacity));
        li.push_back('\0');              // clipping = base
        li.push_back(static_cast<char>(L.visible ? 0x00 : 0x02));  // flags: bit1 set = hidden
        li.push_back('\0');              // filler

        std::string extra;
        put32(extra, 0);                 // layer mask data (none)
        put32(extra, 0);                 // layer blending ranges (none)
        extra += pascal_padded(L.name);  // legacy name (padded to 4)
        // 'lsct' section-divider setting for group markers (1=open folder header,
        // 3=bounding divider "</Layer group>").
        if (L.kind == LayerKind::GroupOpen || L.kind == LayerKind::GroupClose) {
            std::string data;
            put32(data, L.kind == LayerKind::GroupOpen ? 1u : 3u);
            extra += "8BIM";
            extra += "lsct";
            put32(extra, static_cast<uint32_t>(data.size()));
            extra += data;
        }
        put32(li, static_cast<uint32_t>(extra.size()));
        li += extra;
    }
    // Channel image data: per record, per channel -> [compression=0][raw plane]
    // (marker layers are 0x0, so they contribute only the 2-byte compression tag).
    for (const auto& L : layers) {
        const bool isImage = (L.kind == LayerKind::Image);
        for (int c = 0; c < 4; ++c) {
            put16(li, 0);                // raw
            if (isImage && L.width > 0 && L.height > 0 && !L.rgba.empty())
                append_plane(li, L.rgba, L.width, L.height, CH_SRC[c]);
        }
    }
    if (li.size() % 2 != 0) li.push_back('\0');   // layer info padded to even

    // ---- Layer and Mask Information section ----
    std::string lm;
    put32(lm, static_cast<uint32_t>(li.size()));
    lm += li;
    put32(lm, 0);                        // global layer mask info (none)
    put32(f, static_cast<uint32_t>(lm.size()));
    f += lm;

    // ---- Image Data (merged composite), planar R,G,B,A raw ----
    const std::vector<uint8_t> comp = make_composite(docW, docH, layers);
    put16(f, 0);                         // raw
    for (int c = 0; c < 4; ++c)
        append_plane(f, comp, docW, docH, c);

    return f;
}

}  // namespace PsdExport
