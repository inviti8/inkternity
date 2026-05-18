#include "AvatarStore.hpp"

#include <Helpers/Logger.hpp>

#include <include/codec/SkPngDecoder.h>
#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorType.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkStream.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkPngEncoder.h>

#include <cstring>
#include <fstream>
#include <system_error>

namespace AvatarStore {

namespace {

bool write_file_bytes(const std::filesystem::path& path, const uint8_t* data, size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        Logger::get().log("INFO", "[AvatarStore] cannot open for write: " + path.string());
        return false;
    }
    if (size > 0) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// Encode an SkImage to a PNG byte vector. Empty vector on failure.
std::vector<uint8_t> encode_png(const sk_sp<SkImage>& img) {
    if (!img) return {};
    sk_sp<SkImage> raster = img->makeRasterImage(nullptr);
    if (!raster) return {};
    SkPixmap pix;
    if (!raster->peekPixels(&pix)) return {};
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, pix, {})) return {};
    auto data = stream.detachAsData();
    if (!data) return {};
    std::vector<uint8_t> out(data->size());
    std::memcpy(out.data(), data->bytes(), data->size());
    return out;
}

// Downscale an SkImage to side x side using high-quality sampling. The
// input is expected to be already square (capture pipeline produces
// square images); a non-square input gets stretched, which is fine for
// the avatar use case but worth flagging if we ever feed in raw camera
// frames or imported files.
sk_sp<SkImage> downscale_square(const sk_sp<SkImage>& src, int side) {
    if (!src) return nullptr;
    SkImageInfo info = SkImageInfo::Make(side, side, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return nullptr;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SkColor4f{0.0f, 0.0f, 0.0f, 0.0f});
    SkRect dst = SkRect::MakeWH(static_cast<float>(side), static_cast<float>(side));
    SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    canvas->drawImageRect(src, dst, sampling);
    sk_sp<SkImage> snapshot = surface->makeImageSnapshot();
    return snapshot ? snapshot->makeRasterImage(nullptr) : nullptr;
}

}  // namespace

std::filesystem::path master_path(const std::filesystem::path& configPath) {
    return configPath / "avatar.png";
}
std::filesystem::path wire_path(const std::filesystem::path& configPath) {
    return configPath / "avatar_wire.png";
}

bool has_avatar(const std::filesystem::path& configPath) {
    std::error_code ec;
    return std::filesystem::exists(master_path(configPath), ec) && !ec;
}

bool save(const std::filesystem::path& configPath, const sk_sp<SkImage>& master) {
    if (!master) {
        Logger::get().log("INFO", "[AvatarStore] save called with null image");
        return false;
    }
    // Master: encode source directly (already MASTER_SIDE_PX from capture).
    std::vector<uint8_t> masterBytes = encode_png(master);
    if (masterBytes.empty()) {
        Logger::get().log("INFO", "[AvatarStore] master PNG encode failed");
        return false;
    }
    // Wire form: downscale to WIRE_SIDE_PX before encode.
    sk_sp<SkImage> wireImg = downscale_square(master, WIRE_SIDE_PX);
    std::vector<uint8_t> wireBytes = encode_png(wireImg);
    if (wireBytes.empty()) {
        Logger::get().log("INFO", "[AvatarStore] wire PNG encode failed");
        return false;
    }

    if (!write_file_bytes(master_path(configPath), masterBytes.data(), masterBytes.size()))
        return false;
    if (!write_file_bytes(wire_path(configPath), wireBytes.data(), wireBytes.size()))
        return false;
    return true;
}

sk_sp<SkImage> load_master(const std::filesystem::path& configPath) {
    const auto path = master_path(configPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return nullptr;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return nullptr;
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size <= 0) return nullptr;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!f) return nullptr;

    auto data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    if (!data) return nullptr;
    auto codec = SkCodec::MakeFromData(data, {SkPngDecoder::Decoder()});
    if (!codec) return nullptr;
    sk_sp<SkImage> img = std::get<0>(codec->getImage());
    return img ? img->makeRasterImage(nullptr) : nullptr;
}

std::vector<uint8_t> load_wire_bytes(const std::filesystem::path& configPath) {
    const auto path = wire_path(configPath);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!f) return {};
    return bytes;
}

bool clear(const std::filesystem::path& configPath) {
    std::error_code ec;
    std::filesystem::remove(master_path(configPath), ec);
    std::filesystem::remove(wire_path(configPath), ec);
    return true;
}

}  // namespace AvatarStore
