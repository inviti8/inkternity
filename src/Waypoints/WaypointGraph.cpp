#include "WaypointGraph.hpp"
#include "../World.hpp"
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <unordered_map>
#include <vector>
#include <Eigen/Core>

using namespace NetworkingObjects;

WaypointGraph::WaypointGraph(World& w)
    : world(w) {}

void WaypointGraph::register_class(World& w) {
    Waypoint::register_class(w);
    Edge::register_class(w);
}

void WaypointGraph::server_init_no_file() {
    nodes = world.netObjMan.make_obj<NetObjOrderedList<Waypoint>>();
    edges = world.netObjMan.make_obj<NetObjOrderedList<Edge>>();
    layout.clear();
}

void WaypointGraph::read_create_message(cereal::PortableBinaryInputArchive& a) {
    nodes = world.netObjMan.read_create_message<NetObjOrderedList<Waypoint>>(a, nullptr);
    edges = world.netObjMan.read_create_message<NetObjOrderedList<Edge>>(a, nullptr);
    // Layout is local-only state; not transmitted across the network.
}

void WaypointGraph::write_create_message(cereal::PortableBinaryOutputArchive& a) const {
    nodes.write_create_message(a);
    edges.write_create_message(a);
}

void WaypointGraph::scale_up(const WorldScalar& scaleUpAmount) {
    if (nodes) {
        for (auto& n : *nodes)
            n.obj->scale_up(scaleUpAmount);
    }
}

void WaypointGraph::add_edge_enforcing_invariant(NetObjID from,
                                                 NetObjID to,
                                                 std::optional<std::string> label) {
    if (!edges) return;
    // Resolve `from` to a Waypoint to check the transition flag. If the
    // ref is dangling we still append (no enforcement to apply).
    auto fromRef = world.netObjMan.get_obj_temporary_ref_from_id<Waypoint>(from);
    if (fromRef && fromRef->is_transition()) {
        // Walk outgoing edges from `from`; each is a candidate to
        // displace. There should be at most one in a well-formed
        // graph, but defensively erase all of them so a transient
        // invariant violation (e.g. mid-toggle, future multi-user
        // race) collapses to a clean single-out state after this call.
        std::vector<NetObjOrderedListIterator<Edge>> stale;
        for (auto it = edges->begin(); it != edges->end(); ++it) {
            if (it->obj->get_from() == from) stale.push_back(it);
        }
        for (auto& it : stale) edges->erase(edges, it);
    }
    edges->emplace_back_direct(edges, from, to, std::move(label));
}

size_t WaypointGraph::count_outgoing_edges_from(NetObjID from) const {
    if (!edges) return 0;
    size_t n = 0;
    for (auto& info : *edges) {
        if (info.obj->get_from() == from) ++n;
    }
    return n;
}

void WaypointGraph::prune_outgoing_edges_to_first(NetObjID from) {
    if (!edges) return;
    bool keptFirst = false;
    std::vector<NetObjOrderedListIterator<Edge>> toErase;
    for (auto it = edges->begin(); it != edges->end(); ++it) {
        if (it->obj->get_from() != from) continue;
        if (!keptFirst) { keptFirst = true; continue; }
        toErase.push_back(it);
    }
    for (auto& it : toErase) edges->erase(edges, it);
}

void WaypointGraph::erase_waypoint_by_id(NetworkingObjects::NetObjID id) {
    // Erase any edges referencing the waypoint first — the iterators
    // get invalidated after the node erase below, so do this pass first.
    if (edges) {
        std::vector<NetObjOrderedListIterator<Edge>> edgesToErase;
        for (auto it = edges->begin(); it != edges->end(); ++it) {
            if (it->obj->get_from() == id || it->obj->get_to() == id)
                edgesToErase.push_back(it);
        }
        for (auto& it : edgesToErase)
            edges->erase(edges, it);
    }
    // Erase the node itself.
    if (nodes) {
        for (auto it = nodes->begin(); it != nodes->end(); ++it) {
            if (it->obj.get_net_id() == id) {
                nodes->erase(nodes, it);
                break;
            }
        }
    }
    // Drop the layout entry + clear selection if it pointed here.
    layout.erase(id);
    if (selectedId.has_value() && selectedId.value() == id)
        selectedId.reset();
}

void WaypointGraph::save_file(cereal::PortableBinaryOutputArchive& a) const {
    // Build NetObjID -> file-index map so edges and layout entries can
    // reference nodes by stable position rather than by runtime
    // NetObjID (which the loader will reassign).
    std::unordered_map<NetObjID, uint32_t> idToIndex;
    const uint32_t nodeCount = nodes ? static_cast<uint32_t>(nodes->size()) : 0;
    a(nodeCount);
    if (nodes) {
        uint32_t i = 0;
        for (auto& info : *nodes) {
            idToIndex[info.obj.get_net_id()] = i++;
            info.obj->save_file(a);
        }
    }

    const uint32_t edgeCount = edges ? static_cast<uint32_t>(edges->size()) : 0;
    a(edgeCount);
    if (edges) {
        for (auto& info : *edges) {
            const auto fromIt = idToIndex.find(info.obj->get_from());
            const auto toIt   = idToIndex.find(info.obj->get_to());
            const uint32_t fromIdx = (fromIt != idToIndex.end()) ? fromIt->second : 0xFFFFFFFFu;
            const uint32_t toIdx   = (toIt   != idToIndex.end()) ? toIt->second   : 0xFFFFFFFFu;
            a(fromIdx, toIdx);
            info.obj->save_label_file(a);
        }
    }

    // Layout: write count + (idx, x, y) tuples. Entries whose key is
    // not a current node are dropped silently.
    uint32_t layoutCount = 0;
    for (const auto& [id, _] : layout) {
        if (idToIndex.find(id) != idToIndex.end()) ++layoutCount;
    }
    a(layoutCount);
    for (const auto& [id, pos] : layout) {
        const auto it = idToIndex.find(id);
        if (it == idToIndex.end()) continue;
        a(it->second);
        float x = pos.x();
        float y = pos.y();
        a(x, y);
    }
}

void WaypointGraph::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    nodes = world.netObjMan.make_obj<NetObjOrderedList<Waypoint>>();
    edges = world.netObjMan.make_obj<NetObjOrderedList<Edge>>();
    layout.clear();

    // Read nodes: each gets a fresh NetObjID at insertion time. Track
    // the per-index ID so edges and layout can resolve their refs.
    uint32_t nodeCount = 0;
    a(nodeCount);
    std::vector<NetObjID> idsByIndex;
    idsByIndex.reserve(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i) {
        std::string label;
        CoordSpaceHelper coords;
        Vector<int32_t, 2> windowSize{0, 0};
        a(label, coords, windowSize);
        auto it = nodes->emplace_back_direct(nodes, label, coords, windowSize);
        if (version >= VersionNumber(0, 6, 0))
            it->obj->load_skin_from_archive(a, version);
        if (version >= VersionNumber(0, 9, 0))
            it->obj->load_transition_data_from_archive(a, version);
        if (version >= VersionNumber(0, 10, 0))
            it->obj->load_transition_point_data_from_archive(a, version);
        // AUDIO.md §8 — read unconditionally; we don't preserve older
        // saves (the working assumption is "start clean," no migration).
        it->obj->load_audio_data_from_archive(a, version);
        idsByIndex.push_back(it->obj.get_net_id());
    }

    // Read edges via positional indices.
    uint32_t edgeCount = 0;
    a(edgeCount);
    for (uint32_t i = 0; i < edgeCount; ++i) {
        uint32_t fromIdx = 0;
        uint32_t toIdx = 0;
        std::optional<std::string> label;
        a(fromIdx, toIdx);
        a(label);
        if (fromIdx >= idsByIndex.size() || toIdx >= idsByIndex.size()) {
            // Stale or corrupt index — skip the edge rather than referencing
            // a non-node. (A round-trip through current code can't produce
            // this; older saves with deleted-but-not-renumbered edges might.)
            continue;
        }
        edges->emplace_back_direct(edges, idsByIndex[fromIdx], idsByIndex[toIdx], std::move(label));
    }

    // Read layout map.
    uint32_t layoutCount = 0;
    a(layoutCount);
    for (uint32_t i = 0; i < layoutCount; ++i) {
        uint32_t idx = 0;
        float x = 0.0f, y = 0.0f;
        a(idx, x, y);
        if (idx >= idsByIndex.size()) continue;
        layout.emplace(idsByIndex[idx], Vector2f{x, y});
    }
}
