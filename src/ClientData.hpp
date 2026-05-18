#pragma once
#include <Helpers/Serializers.hpp>
#include "CoordSpaceHelper.hpp"
#include <Helpers/NetworkingObjects/NetObjTemporaryPtr.hpp>
#include <Helpers/NetworkingObjects/NetObjManager.hpp>

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <cstdint>
#include <vector>

class ClientData {
    public:
        struct InitStruct {
            CoordSpaceHelper camCoords;
            Vector2f windowSize = {1000.0f, 1000.0f};
            Vector2f cursorPos = {1.0f, 1.0f};

            Vector3f cursorColor = {1.0f, 1.0f, 1.0f};
            std::string displayName;
            uint32_t gridSize = 0;
            // P0-C8 / P0-D4: subscriber-vs-collaborator marker. Set
            // true by the host when a client passes the five-check
            // token verification (sub-only mode); false otherwise
            // (vanilla collab join). Used for viewer-mode UI gating.
            bool isViewer = false;
            // PHASE3 §4 B.M4 -- 64x64 PNG wire form of the artist's
            // avatar. Empty when the artist has never captured one;
            // receiver falls back to the existing colored-circle
            // cursor rendering. Populated on join by loading the local
            // avatar_wire.png via AvatarStore::load_wire_bytes.
            std::vector<uint8_t> avatarPng;
        };

        ClientData();
        ClientData(const InitStruct& initStruct);
        static void register_class(World& world);
        static void set_cursor_pos(const NetworkingObjects::NetObjTemporaryPtr<ClientData>& o, Vector2f newPos);
        static void set_window_size(const NetworkingObjects::NetObjTemporaryPtr<ClientData>& o, Vector2f newWindowSize);
        static void set_camera_coords(const NetworkingObjects::NetObjTemporaryPtr<ClientData>& o, const CoordSpaceHelper& newCoords);
        static void send_chat_message(const NetworkingObjects::NetObjTemporaryPtr<ClientData>& o, World& world, const std::string& chatMessage);
        static void scale_up_step(const NetworkingObjects::NetObjTemporaryPtr<ClientData>& o, World& world);
        void set_display_name(const std::string& newDisplayName);
        void set_cursor_color(const Vector3f& newCursorColor);

        const CoordSpaceHelper& get_cam_coords() const;
        const Vector2f& get_window_size() const;
        const Vector2f& get_cursor_pos() const;
        uint32_t get_grid_size() const;
        const std::string& get_display_name() const;
        const Vector3f& get_cursor_color() const;
        bool is_viewer() const { return isViewer; }

        void draw_cursor(SkCanvas* canvas, const DrawData& drawData) const;

        template <typename Archive> void serialize(Archive& a) {
            a(camCoords, windowSize, cursorPos, cursorColor, displayName, gridSize, isViewer, avatarPng);
        }
    private:
        void set_from_init_struct(const InitStruct& initStruct);
        // Lazy PNG-decode of avatarPng into avatarImageCache. Idempotent;
        // returns the cache or nullptr if avatarPng is empty / decode
        // fails. Called from the const draw_cursor path, hence mutable.
        const sk_sp<SkImage>& ensure_avatar_decoded() const;

        CoordSpaceHelper camCoords;
        Vector2f windowSize;
        Vector2f cursorPos;

        Vector3f cursorColor;
        std::string displayName;
        uint32_t gridSize;
        bool isViewer = false;
        // PHASE3 §4 B.M4 -- received PNG bytes + lazy decode cache.
        // Wire form is 64x64; cache is decoded once per peer and held
        // for the session.
        std::vector<uint8_t> avatarPng;
        mutable sk_sp<SkImage> avatarImageCache;
        mutable bool avatarDecodeTried = false;
};

