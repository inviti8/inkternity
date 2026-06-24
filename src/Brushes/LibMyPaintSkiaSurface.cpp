#include "LibMyPaintSkiaSurface.hpp"

#ifdef HVYM_HAS_LIBMYPAINT

#include <algorithm>
#include <cstring>
#include <limits>

#include <cereal/types/vector.hpp>

#include <include/core/SkBitmap.h>
#include <include/core/SkImageInfo.h>

namespace HVYM::Brushes {

namespace {

// libmypaint stores tiles as 16-bit-per-channel premultiplied RGBA. Mirrors
// the conversion used in the M1 hello-dab spike (LibMyPaintBridgeTest.cpp).
inline uint8_t channel_to_8bit_unpremul(uint16_t c, uint16_t a) {
    if (a == 0) return 0;
    uint32_t v = (static_cast<uint32_t>(c) * 0xFFFF) / a;
    if (v > 0xFFFF) v = 0xFFFF;
    return static_cast<uint8_t>(v >> 8);
}

constexpr size_t kChannelsPerPx = 4;
constexpr size_t kPixelsPerTile = static_cast<size_t>(LibMyPaintSkiaSurface::kTilePx) * LibMyPaintSkiaSurface::kTilePx;
constexpr size_t kU16PerTile = kPixelsPerTile * kChannelsPerPx;

}  // namespace

LibMyPaintSkiaSurface::LibMyPaintSkiaSurface() {
    mypaint_tiled_surface_init(&tiled_, &s_tile_request_start, &s_tile_request_end);
}

LibMyPaintSkiaSurface::~LibMyPaintSkiaSurface() {
    mypaint_tiled_surface_destroy(&tiled_);
}

void LibMyPaintSkiaSurface::s_tile_request_start(MyPaintTiledSurface* tiled, MyPaintTileRequest* req) {
    auto* self = reinterpret_cast<LibMyPaintSkiaSurface*>(tiled);
    self->on_tile_request_start(req);
}

void LibMyPaintSkiaSurface::s_tile_request_end(MyPaintTiledSurface*, MyPaintTileRequest*) {
    // Tiles are stored directly; libmypaint wrote in place. Nothing to do.
}

void LibMyPaintSkiaSurface::on_tile_request_start(MyPaintTileRequest* req) {
    const TileKey key{req->tx, req->ty};
    auto it = tiles_.find(key);
    if (it == tiles_.end()) {
        // Read-only requests against an unallocated tile must still hand back
        // a real buffer (libmypaint reads the destination color). Allocate
        // and store it so subsequent writes find the same memory.
        auto buf = std::make_unique<uint16_t[]>(kU16PerTile);
        std::memset(buf.get(), 0, kU16PerTile * sizeof(uint16_t));
        it = tiles_.emplace(key, std::move(buf)).first;
    }
    req->buffer = it->second.get();
}

LibMyPaintSkiaSurface::PxRect LibMyPaintSkiaSurface::allocated_tile_bounds_px() const {
    if (tiles_.empty()) return {0, 0, 0, 0};
    int tx_min = std::numeric_limits<int>::max();
    int ty_min = std::numeric_limits<int>::max();
    int tx_max = std::numeric_limits<int>::min();
    int ty_max = std::numeric_limits<int>::min();
    for (const auto& [key, _] : tiles_) {
        tx_min = std::min(tx_min, key.tx);
        ty_min = std::min(ty_min, key.ty);
        tx_max = std::max(tx_max, key.tx);
        ty_max = std::max(ty_max, key.ty);
    }
    return {
        tx_min * kTilePx,
        ty_min * kTilePx,
        (tx_max - tx_min + 1) * kTilePx,
        (ty_max - ty_min + 1) * kTilePx,
    };
}

bool LibMyPaintSkiaSurface::has_any_tile_in_pixel_rect(int pxX0, int pxY0, int pxW, int pxH) const {
    if (tiles_.empty() || pxW <= 0 || pxH <= 0) return false;
    // Convert inclusive pixel rect to inclusive tile-index rect via
    // floor-div that's correct for negative coordinates.
    auto floor_div = [](int a, int b) {
        int q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    };
    const int tx0 = floor_div(pxX0, kTilePx);
    const int ty0 = floor_div(pxY0, kTilePx);
    const int tx1 = floor_div(pxX0 + pxW - 1, kTilePx);
    const int ty1 = floor_div(pxY0 + pxH - 1, kTilePx);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tiles_.find(TileKey{tx, ty}) != tiles_.end()) return true;
        }
    }
    return false;
}

void LibMyPaintSkiaSurface::composite_to_bitmap(SkBitmap& dst, int dstOriginPxX, int dstOriginPxY) const {
    if (dst.colorType() != kRGBA_8888_SkColorType) return;
    uint8_t* dstPixels = static_cast<uint8_t*>(dst.getPixels());
    if (!dstPixels) return;
    const int dstW = dst.width();
    const int dstH = dst.height();
    const size_t dstRowBytes = dst.rowBytes();

    for (const auto& [key, buf] : tiles_) {
        const int tilePxX = key.tx * kTilePx;
        const int tilePxY = key.ty * kTilePx;
        const int dstX = tilePxX - dstOriginPxX;
        const int dstY = tilePxY - dstOriginPxY;

        const int copyX0 = std::max(0, dstX);
        const int copyY0 = std::max(0, dstY);
        const int copyX1 = std::min(dstW, dstX + kTilePx);
        const int copyY1 = std::min(dstH, dstY + kTilePx);
        if (copyX0 >= copyX1 || copyY0 >= copyY1) continue;

        const uint16_t* tile = buf.get();
        for (int row = copyY0; row < copyY1; ++row) {
            const int srcRowOffset = (row - dstY) * kTilePx * 4;
            uint8_t* dstRow = dstPixels + row * dstRowBytes + copyX0 * 4;
            const uint16_t* srcRow = tile + srcRowOffset + (copyX0 - dstX) * 4;
            for (int col = copyX0; col < copyX1; ++col) {
                const uint16_t r = srcRow[0];
                const uint16_t g = srcRow[1];
                const uint16_t b = srcRow[2];
                const uint16_t a = srcRow[3];
                dstRow[0] = channel_to_8bit_unpremul(r, a);
                dstRow[1] = channel_to_8bit_unpremul(g, a);
                dstRow[2] = channel_to_8bit_unpremul(b, a);
                dstRow[3] = static_cast<uint8_t>(a >> 8);
                srcRow += 4;
                dstRow += 4;
            }
        }
    }
}

void LibMyPaintSkiaSurface::save_tiles_to_archive(cereal::PortableBinaryOutputArchive& a) const {
    const uint32_t count = static_cast<uint32_t>(tiles_.size());
    a(count);
    constexpr size_t bytesPerTile = kU16PerTile * sizeof(uint16_t);
    for (const auto& [key, buf] : tiles_) {
        const int32_t tx = key.tx;
        const int32_t ty = key.ty;
        a(tx, ty);
        // Write the raw tile bytes via cereal's binary_data helper —
        // matches how cereal serializes PODs without per-element overhead.
        a(cereal::binary_data(buf.get(), bytesPerTile));
    }
}

void LibMyPaintSkiaSurface::load_tiles_from_archive(cereal::PortableBinaryInputArchive& a) {
    tiles_.clear();
    uint32_t count = 0;
    a(count);
    constexpr size_t bytesPerTile = kU16PerTile * sizeof(uint16_t);
    for (uint32_t i = 0; i < count; ++i) {
        int32_t tx = 0, ty = 0;
        a(tx, ty);
        auto buf = std::make_unique<uint16_t[]>(kU16PerTile);
        a(cereal::binary_data(buf.get(), bytesPerTile));
        tiles_.emplace(TileKey{tx, ty}, std::move(buf));
    }
}

void LibMyPaintSkiaSurface::copy_tiles_from(const LibMyPaintSkiaSurface& other) {
    tiles_.clear();
    for (const auto& [key, srcBuf] : other.tiles_) {
        auto buf = std::make_unique<uint16_t[]>(kU16PerTile);
        std::memcpy(buf.get(), srcBuf.get(), kU16PerTile * sizeof(uint16_t));
        tiles_.emplace(key, std::move(buf));
    }
}

void LibMyPaintSkiaSurface::build_halved_from(const LibMyPaintSkiaSurface& src) {
    // LAYER-RESOLUTION.md §"Resample math". Each source tile (tx,ty)
    // downsamples to exactly one 32x32 quadrant of dest tile
    // (floor(tx/2), floor(ty/2)) — no cross-tile sampling, because 64 is
    // even so source pixels [tx*64, tx*64+64) map to dest pixels
    // [floor(tx/2)*64 + (tx&1)*32, ...+32). We average 2x2 premultiplied
    // texels (a straight per-channel mean is the correct box filter in
    // premultiplied space).
    constexpr int half = kTilePx / 2;  // 32: dest pixels each source tile feeds
    tiles_.clear();

    auto floor_div2 = [](int a) {  // floor(a/2), correct for negative a
        return (a >= 0) ? (a >> 1) : -(((-a) + 1) >> 1);
    };

    for (const auto& [key, srcBufPtr] : src.tiles_) {
        const int DX = floor_div2(key.tx);
        const int DY = floor_div2(key.ty);
        const int qx = key.tx - DX * 2;  // 0 or 1 (floor remainder, neg-safe)
        const int qy = key.ty - DY * 2;

        const TileKey dkey{DX, DY};
        auto it = tiles_.find(dkey);
        if (it == tiles_.end()) {
            auto buf = std::make_unique<uint16_t[]>(kU16PerTile);
            std::memset(buf.get(), 0, kU16PerTile * sizeof(uint16_t));
            it = tiles_.emplace(dkey, std::move(buf)).first;
        }
        uint16_t* dst = it->second.get();
        const uint16_t* s = srcBufPtr.get();

        const int baseLX = qx * half;  // quadrant origin within the dest tile
        const int baseLY = qy * half;
        for (int dy = 0; dy < half; ++dy) {
            const int sy = dy * 2;
            const uint16_t* sRow0 = s + (static_cast<size_t>(sy) * kTilePx) * 4;
            const uint16_t* sRow1 = sRow0 + kTilePx * 4;  // next source row
            uint16_t* dRow = dst + (static_cast<size_t>(baseLY + dy) * kTilePx + baseLX) * 4;
            for (int dx = 0; dx < half; ++dx) {
                const uint16_t* a = sRow0 + (dx * 2) * 4;  // (sx,   sy)
                const uint16_t* b = a + 4;                 // (sx+1, sy)
                const uint16_t* c = sRow1 + (dx * 2) * 4;  // (sx,   sy+1)
                const uint16_t* d = c + 4;                 // (sx+1, sy+1)
                for (int ch = 0; ch < 4; ++ch) {
                    const uint32_t sum = static_cast<uint32_t>(a[ch]) + b[ch] + c[ch] + d[ch];
                    dRow[ch] = static_cast<uint16_t>(sum >> 2);
                }
                dRow += 4;
            }
        }
    }

    // Drop dest tiles that came out fully transparent (e.g. a dest tile whose
    // only contributing source quadrant was transparent), preserving sparsity.
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        const uint16_t* buf = it->second.get();
        bool anyOpaque = false;
        for (size_t i = 3; i < kU16PerTile; i += 4) {
            if (buf[i] != 0) { anyOpaque = true; break; }
        }
        if (anyOpaque) ++it;
        else it = tiles_.erase(it);
    }
}

namespace {
// Inverse of channel_to_8bit_unpremul. channel_to_8bit_unpremul does
// a8 = a16 >> 8 and cu8 = ((c16 * 0xFFFF) / a16) >> 8. Using the 8->16
// expansion v16 = v8 * 257 (0xFF -> 0xFFFF) makes the round-trip exact:
//   a16  = a8 * 257
//   c16  = (cu8 * 257) * a16 / 0xFFFF      (premultiply)
// then composite recovers a8 and cu8 with no drift (verified algebraically).
inline uint16_t channel_to_16bit_premul(uint8_t cu8, uint32_t a16) {
    const uint32_t cu16 = static_cast<uint32_t>(cu8) * 257u;
    return static_cast<uint16_t>((cu16 * a16) / 0xFFFFu);
}

inline int floor_div_tile(int a, int b) {
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
}  // namespace

void LibMyPaintSkiaSurface::import_from_bitmap(const SkBitmap& src, int srcOriginPxX, int srcOriginPxY) {
    if (src.colorType() != kRGBA_8888_SkColorType) return;
    const uint8_t* srcPixels = static_cast<const uint8_t*>(src.getPixels());
    if (!srcPixels) return;
    const int srcW = src.width();
    const int srcH = src.height();
    if (srcW <= 0 || srcH <= 0) return;
    const size_t srcRowBytes = src.rowBytes();

    // Tile-index range covering the source rect in surface-local space.
    const int tx0 = floor_div_tile(srcOriginPxX, kTilePx);
    const int ty0 = floor_div_tile(srcOriginPxY, kTilePx);
    const int tx1 = floor_div_tile(srcOriginPxX + srcW - 1, kTilePx);
    const int ty1 = floor_div_tile(srcOriginPxY + srcH - 1, kTilePx);

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int tilePxX = tx * kTilePx;
            const int tilePxY = ty * kTilePx;
            // Overlap of this tile with the source rect, in source pixel coords.
            const int sx0 = std::max(0, tilePxX - srcOriginPxX);
            const int sy0 = std::max(0, tilePxY - srcOriginPxY);
            const int sx1 = std::min(srcW, tilePxX + kTilePx - srcOriginPxX);
            const int sy1 = std::min(srcH, tilePxY + kTilePx - srcOriginPxY);
            if (sx0 >= sx1 || sy0 >= sy1) continue;

            // Skip allocating a tile the source only covers transparently —
            // keeps the tile store sparse (the whole point of flattening).
            bool anyOpaque = false;
            for (int sy = sy0; sy < sy1 && !anyOpaque; ++sy) {
                const uint8_t* row = srcPixels + sy * srcRowBytes + sx0 * 4;
                for (int sx = sx0; sx < sx1; ++sx) {
                    if (row[3] != 0) { anyOpaque = true; break; }
                    row += 4;
                }
            }
            if (!anyOpaque) continue;

            const TileKey key{tx, ty};
            auto it = tiles_.find(key);
            if (it == tiles_.end()) {
                auto buf = std::make_unique<uint16_t[]>(kU16PerTile);
                std::memset(buf.get(), 0, kU16PerTile * sizeof(uint16_t));
                it = tiles_.emplace(key, std::move(buf)).first;
            }
            uint16_t* tile = it->second.get();

            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* srcRow = srcPixels + sy * srcRowBytes + sx0 * 4;
                const int ly = (srcOriginPxY + sy) - tilePxY;           // 0..kTilePx-1
                const int lx0 = (srcOriginPxX + sx0) - tilePxX;
                uint16_t* dstRow = tile + (static_cast<size_t>(ly) * kTilePx + lx0) * 4;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t a16 = static_cast<uint32_t>(srcRow[3]) * 257u;
                    dstRow[0] = channel_to_16bit_premul(srcRow[0], a16);
                    dstRow[1] = channel_to_16bit_premul(srcRow[1], a16);
                    dstRow[2] = channel_to_16bit_premul(srcRow[2], a16);
                    dstRow[3] = static_cast<uint16_t>(a16);
                    srcRow += 4;
                    dstRow += 4;
                }
            }
        }
    }
}

size_t LibMyPaintSkiaSurface::drop_fully_transparent_tiles_in_pixel_rect(int pxX0, int pxY0, int pxW, int pxH) {
    if (tiles_.empty() || pxW <= 0 || pxH <= 0) return 0;
    const int tx0 = floor_div_tile(pxX0, kTilePx);
    const int ty0 = floor_div_tile(pxY0, kTilePx);
    const int tx1 = floor_div_tile(pxX0 + pxW - 1, kTilePx);
    const int ty1 = floor_div_tile(pxY0 + pxH - 1, kTilePx);
    size_t freed = 0;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            auto it = tiles_.find(TileKey{tx, ty});
            if (it == tiles_.end()) continue;
            const uint16_t* buf = it->second.get();
            bool anyOpaque = false;
            for (size_t i = 3; i < kU16PerTile; i += 4) {  // alpha channel, stride 4
                if (buf[i] != 0) { anyOpaque = true; break; }
            }
            if (!anyOpaque) {
                tiles_.erase(it);
                ++freed;
            }
        }
    }
    return freed;
}

}  // namespace HVYM::Brushes

#endif  // HVYM_HAS_LIBMYPAINT
