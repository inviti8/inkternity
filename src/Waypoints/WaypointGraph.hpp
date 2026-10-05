#pragma once
#include "Waypoint.hpp"
#include "Edge.hpp"
#include <Helpers/NetworkingObjects/NetObjOwnerPtr.hpp>
#include <Helpers/NetworkingObjects/NetObjOrderedList.hpp>
#include <Helpers/VersionNumber.hpp>
#include <Eigen/Core>
#include <cereal/archives/portable_binary.hpp>
#include <optional>
#include <unordered_map>

class World;

// PHASE1.md §5 — owns the flat list of Waypoint nodes plus the directed
// Edge list between them, and a per-node layout (positions in the tree
// window). The layout is a local UI state field but is serialized so a
// reload preserves the user's tree-window arrangement; it is NOT
// NetObj-synced (each client is free to lay the graph out differently).
//
// Lives on World alongside BookmarkManager. Per §5 the bookmark side
// panel is removed in M5/M6 and BookmarkManager becomes a thin shim
// that round-trips through this graph; for M4 the two coexist
// independently and migration of existing bookmarks lands as M4-b.
class WaypointGraph {
    public:
        explicit WaypointGraph(World& w);

        void server_init_no_file();
        void read_create_message(cereal::PortableBinaryInputArchive& a);
        void write_create_message(cereal::PortableBinaryOutputArchive& a) const;

        void scale_up(const WorldScalar& scaleUpAmount);

        void save_file(cereal::PortableBinaryOutputArchive& a) const;
        void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version);

        static void register_class(World& w);

        const NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<Waypoint>>& get_nodes() const { return nodes; }
        const NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<Edge>>& get_edges() const { return edges; }
        const std::unordered_map<NetworkingObjects::NetObjID, Vector2f>& get_layout() const { return layout; }
        std::unordered_map<NetworkingObjects::NetObjID, Vector2f>& mutable_layout() { return layout; }

        // UI selection — shared between WaypointTool (canvas) and
        // TreeView (graph panel) so a click in either updates the
        // other's chrome. Local-only state (not NetObj-synced or
        // persisted), since selection is per-user-session.
        bool has_selection() const                         { return selectedId.has_value(); }
        NetworkingObjects::NetObjID get_selected() const   { return selectedId.value_or(NetworkingObjects::NetObjID{}); }
        void select(NetworkingObjects::NetObjID id)        { selectedId = id; }
        void clear_selection()                             { selectedId.reset(); }

        // PHASE2 polish: erases the Waypoint with the given NetObjID,
        // any edges that reference it, its layout entry, and clears
        // selection if it pointed at this waypoint. No-op if the id
        // doesn't match a known node. Called from the layer-component
        // erase callback so any path that removes the on-canvas
        // WaypointCanvasComponent (undo, eraser, layer delete) also
        // sweeps the graph-side state.
        void erase_waypoint_by_id(NetworkingObjects::NetObjID id);

        // TRANSITIONS.md T5 — adds an edge while enforcing the
        // "transition point has at most one outgoing edge" invariant.
        // If `from` resolves to a transition Waypoint that already has
        // an outgoing edge, that edge is erased first; then the new
        // edge is appended. Non-transition `from` falls through to a
        // plain append. File-load and any other low-level path that
        // needs to bypass the invariant should still call
        // `edges->emplace_back_direct` directly.
        void add_edge_enforcing_invariant(NetworkingObjects::NetObjID from,
                                          NetworkingObjects::NetObjID to,
                                          std::optional<std::string> label);

        // TRANSITIONS.md T6 — counts outgoing edges from `from`. Used
        // by WaypointTool to decide whether to surface the multi-out
        // toggle-on cleanup prompt.
        size_t count_outgoing_edges_from(NetworkingObjects::NetObjID from) const;

        // TRANSITIONS.md T6 — keeps the first outgoing edge from
        // `from` (in graph order) and erases every other one. The
        // user-facing "Keep first" cleanup action is the trigger.
        void prune_outgoing_edges_to_first(NetworkingObjects::NetObjID from);

        // Marker→node linkage (INFPNT000035). WaypointCanvasComponent markers
        // reference a node by NetObjID, but node ids are reassigned on load, so the
        // link is persisted as a positional index (like edges). node_index_of() maps a
        // live node id to its position for save; loaded_node_ids_by_index() returns the
        // post-load id for each saved position so World can re-link markers.
        int node_index_of(NetworkingObjects::NetObjID id) const;
        const std::vector<NetworkingObjects::NetObjID>& loaded_node_ids_by_index() const { return loadedNodeIdsByIndex; }

    private:
        World& world;
        // Populated by load_file: index (as written on disk) -> fresh node NetObjID.
        std::vector<NetworkingObjects::NetObjID> loadedNodeIdsByIndex;

        NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<Waypoint>> nodes;
        NetworkingObjects::NetObjOwnerPtr<NetworkingObjects::NetObjOrderedList<Edge>> edges;
        std::unordered_map<NetworkingObjects::NetObjID, Vector2f> layout;
        std::optional<NetworkingObjects::NetObjID> selectedId;
};
