#include "FxLibraryStore.hpp"

#ifdef HVYM_HAS_TIMELINEFX_LEGACY

#include "LegacyFxLibrary.hpp"

FxLibraryStore::FxLibraryStore() = default;
FxLibraryStore::~FxLibraryStore() = default;

int FxLibraryStore::add(const NetworkingObjects::NetObjID& resourceId, const std::string& name,
                        std::unique_ptr<LegacyFxLibrary> lib) {
    for (size_t i = 0; i < _entries.size(); ++i)
        if (_entries[i].resourceId == resourceId) return static_cast<int>(i);
    _entries.push_back(Entry{resourceId, name, std::move(lib)});
    return static_cast<int>(_entries.size()) - 1;
}

void FxLibraryStore::setActive(int libraryIndex, const std::string& effectName) {
    _activeLibrary = libraryIndex;
    _activeEffect = effectName;
}

bool FxLibraryStore::isActive(int libraryIndex, const std::string& effectName) const {
    return _activeLibrary == libraryIndex && _activeEffect == effectName;
}

bool FxLibraryStore::hasActive() const {
    return _activeLibrary >= 0 && _activeLibrary < static_cast<int>(_entries.size())
        && !_activeEffect.empty();
}

#endif // HVYM_HAS_TIMELINEFX_LEGACY
