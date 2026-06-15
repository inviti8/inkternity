#pragma once

// In-session store of imported legacy FX libraries (.eff), keyed by the
// ResourceManager NetObjID of the embedded bytes. Holds the parsed
// LegacyFxLibrary per import and the single active (library, effect) selection
// that the FX Library panel sets and the particle brush (M2) will read.
// PHASE5.1 M1. Gated by HVYM_HAS_TIMELINEFX_LEGACY.

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

#include "Helpers/NetworkingObjects/NetObjID.hpp"

class LegacyFxLibrary;

class FxLibraryStore {
public:
    struct Entry {
        NetworkingObjects::NetObjID resourceId;     // the embedded .eff asset
        std::string name;                           // .eff filename (display)
        std::unique_ptr<LegacyFxLibrary> lib;       // parsed library
        // Lazily headless-rendered panel thumbnails, keyed by effect name
        // (a cached entry may be null = render failed/empty — don't retry).
        std::unordered_map<std::string, sk_sp<SkImage>> thumbs;
    };

    FxLibraryStore();
    ~FxLibraryStore();   // out-of-line: Entry holds unique_ptr<incomplete>

    // Adopt an already-parsed library. Dedups by resourceId (returns the
    // existing index if already present). Returns the entry index.
    int add(const NetworkingObjects::NetObjID& resourceId, const std::string& name,
            std::unique_ptr<LegacyFxLibrary> lib);

    const std::vector<Entry>& entries() const { return _entries; }
    bool empty() const { return _entries.empty(); }

    // A small thumbnail of an effect at its peak-population frame (headless
    // render via the M3 renderer, the proven spike approach). Generated on
    // first request and cached per (library, effect). Returns null if the
    // effect is missing or produced nothing.
    sk_sp<SkImage> thumbnail(int libraryIndex, const std::string& effectName);

    // Whether a thumbnail has already been generated+cached (so the caller can
    // budget generation across frames — generating is ~tens of ms each).
    bool thumbnail_cached(int libraryIndex, const std::string& effectName) const;

    // Single active (library, effect) selection — §4.4.
    void setActive(int libraryIndex, const std::string& effectName);
    int activeLibrary() const { return _activeLibrary; }
    const std::string& activeEffect() const { return _activeEffect; }
    bool isActive(int libraryIndex, const std::string& effectName) const;
    bool hasActive() const;

private:
    std::vector<Entry> _entries;
    int _activeLibrary = -1;
    std::string _activeEffect;
};

#endif // HVYM_HAS_TIMELINEFX_LEGACY
