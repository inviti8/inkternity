#include "LegacyFxLibrary.hpp"

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include "miniz.h"

#include "TLFXPugiXMLLoader.h"

#include <include/codec/SkCodec.h>
#include <include/codec/SkPngDecoder.h>

bool LegacyAnimImage::Load(const char* filename) {
    sk_sp<SkData> bytes = _owner->shapeBytes(filename);
    if (!bytes) return false;
    std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(bytes, nullptr);
    if (!codec) return false;
    auto [decoded, res] = codec->getImage();
    image = decoded;
    return image != nullptr;
}

namespace {

// PugiXMLLoader reads data.xml from a file (load_file); feed it the in-memory
// buffer instead (load_buffer), replicating Open()'s cursor setup.
class MemPugiXMLLoader : public TLFX::PugiXMLLoader {
public:
    MemPugiXMLLoader(int shapes, const std::string& xml)
        : TLFX::PugiXMLLoader(shapes), _xml(xml) {}

    bool Open(const char* /*filename*/) override {
        _error[0] = 0;
        pugi::xml_parse_result result = _doc.load_buffer(_xml.data(), _xml.size());
        if (!result) {
            std::snprintf(_error, sizeof(_error), "parse error: %s", result.description());
            return false;
        }
        if (!_doc.child("EFFECTS")) {
            std::snprintf(_error, sizeof(_error), "root <EFFECTS> missing");
            return false;
        }
        _currentShape = _doc.child("EFFECTS").child("SHAPES").child("IMAGE");
        _currentFolder = _doc.child("EFFECTS").child("FOLDER");
        while (!_currentEffect && _currentFolder) {
            _currentEffect = _currentFolder.child("EFFECT");
            if (!_currentEffect) _currentFolder = _currentFolder.next_sibling("FOLDER");
        }
        if (!_currentEffect) _currentEffect = _doc.child("EFFECTS").child("EFFECT");
        return true;
    }

private:
    const std::string& _xml;
};

} // namespace

TLFX::XMLLoader* LegacyFxLibrary::CreateLoader() const {
    return new MemPugiXMLLoader(static_cast<int>(_shapeList.size()), _dataXml);
}

TLFX::AnimImage* LegacyFxLibrary::CreateImage() const {
    return new LegacyAnimImage(this);
}

sk_sp<SkData> LegacyFxLibrary::shapeBytes(const std::string& filename) const {
    auto it = _shapeBytes.find(filename);
    return it == _shapeBytes.end() ? nullptr : it->second;
}

std::vector<std::string> LegacyFxLibrary::topLevelEffectNames() const {
    std::vector<std::string> v;
    for (auto& kv : _effects)
        if (kv.first.find('/') == std::string::npos) v.push_back(kv.first);
    return v;
}

bool LegacyFxLibrary::loadFromEff(const void* effZipBytes, size_t size) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, effZipBytes, size, 0)) return false;

    mz_uint nfiles = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < nfiles; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!p) continue;

        std::string name = st.m_filename;
        std::string base = name.substr(name.find_last_of("/\\") + 1);
        if (base == "data.xml") {
            _dataXml.assign(static_cast<const char*>(p), outSize);
        } else {
            std::string ext;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) ext = base.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png") _shapeBytes[base] = SkData::MakeWithCopy(p, outSize);
        }
        mz_free(p);
    }
    mz_zip_reader_end(&zip);

    if (_dataXml.empty()) return false;
    return Load("<memory>");   // EffectsLibrary::Load -> CreateLoader/CreateImage
}

#endif // HVYM_HAS_TIMELINEFX_LEGACY
