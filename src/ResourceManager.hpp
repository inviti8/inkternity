#pragma once
#include <Helpers/NetworkingObjects/NetObjOwnerPtr.hpp>
#include <include/codec/SkCodec.h>
#include "Helpers/NetworkingObjects/NetObjID.hpp"
#include "ResourceDisplay/ResourceDisplay.hpp"
#include <Helpers/Networking/NetLibrary.hpp>
#include <Helpers/NetworkingObjects/NetObjUnorderedSet.hpp>
#include <Helpers/VersionNumber.hpp>

class World;

struct ResourceData {
    std::string name;
    std::shared_ptr<std::string> data;
    template <typename Archive> void save(Archive& a) const {
        a(name, *data);
    }
    template <typename Archive> void load(Archive& a) {
        data = std::make_shared<std::string>();
        a(name, *data);
    }
};

class ResourceManager {
    public:
        ResourceManager(World& initWorld);
        void update();
        void init_client_callbacks();
        void init_server_callbacks();
        NetworkingObjects::NetObjTemporaryPtr<ResourceData> add_resource_file(const std::filesystem::path& filePath);
        const NetworkingObjects::NetObjOwnerPtr<ResourceData>& add_resource(const ResourceData& resource);
        ResourceDisplay* get_display_data(const NetworkingObjects::NetObjID& fileID);
        const std::vector<NetworkingObjects::NetObjOwnerPtr<ResourceData>>& resource_list();
        float get_resource_retrieval_progress(const NetworkingObjects::NetObjID& id);
        std::unordered_map<NetworkingObjects::NetObjID, ResourceData> copy_resource_set_to_map(const std::unordered_set<NetworkingObjects::NetObjID>& resourceSet) const;
        // Layer-group import: register resources bundled with an imported group
        // subtree under their ORIGINAL ids (component data references resources
        // by raw NetObjID, so the id must be preserved for the reference to
        // resolve). IDs already present in this canvas are skipped — the
        // existing resource is shared, which also makes re-importing the same
        // group twice safe. Mirrors load_file's emplace, minus the collision.
        void import_resource_map(const std::unordered_map<NetworkingObjects::NetObjID, ResourceData>& resources);
        void clear_display_cache();

        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version);
        void save_file(cereal::PortableBinaryOutputArchive& a) const;

        World& world;
    private:
        std::unordered_map<NetworkingObjects::NetObjID, std::unique_ptr<ResourceDisplay>> displays;

        std::unordered_map<NetworkingObjects::NetObjID, std::weak_ptr<NetServer::ClientData>> resourcesBeingRetrieved;
        std::vector<NetworkingObjects::NetObjOwnerPtr<ResourceData>> resourceList;

        static std::array<const SkCodecs::Decoder, 5> decoders;
};

