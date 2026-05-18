#pragma once

#include <string>
#include <vector>

#ifdef HVYM_HAS_LIBMYPAINT
#include "Brushes/BrushPresets.hpp"
#endif

class Toolbar;

// PHASE3.md §3 A2.M2 -- Saved Presets drawer popup body.
//
// Browse the curated set + every user-saved preset under
// <configPath>/brush_presets/. Click a tile to activate that preset on
// the live MyPaintBrush. Curated tile click sets cfg.activePresetIndex
// (existing flow); user tile click sets cfg.activeUserPresetPath
// (added in this milestone), which apply_active_preset_with_overrides
// honors per-stroke.
//
// Scope of this milestone (A2.M2):
//   - Grid of tiles, grouped by category (Sharp / Textured).
//   - Each tile: preset name centered. Icon thumbnails are deferred --
//     no in-tree memory-image widget yet; curated presets ship SVG
//     icons (Toolbar's brush picker style); user presets get the
//     identicon fallback (A.M-identicon milestone). For v1, name-only.
//   - Search field filters tiles by case-insensitive substring match.
//   - Click activates the preset. The customization drawer reflects
//     the new state next time it's opened.
//   - Disk scan happens each frame the drawer is rendering (cheap
//     enough -- the user-preset count is bounded by what an artist
//     hand-saves).
//
// Out of scope (other A2 sub-milestones):
//   - Per-tile context menu (Rename / Edit / Delete / Duplicate) -> A2.M3
//   - Drag-reorder + move-between-categories                       -> A2.M4
//   - Per-tile icon rendering                                      -> polish
//   - Phone-UI parity                                              -> A.M-phone
class SavedPresetsDrawer {
    public:
        explicit SavedPresetsDrawer(Toolbar& toolbar);

        // Render the popup body inside the caller's Clay scope. No-op
        // when no MyPaintBrushTool is active (toolbar icon hides too).
        void render_body();

    private:
#ifdef HVYM_HAS_LIBMYPAINT
        std::string searchQuery_;
        // Curated presets are immutable + always the same handful, so
        // we read them on every render via curated_presets(). User
        // presets come from disk -- cached per drawer-open to avoid
        // re-scanning the filesystem on every render frame.
        std::vector<HVYM::Brushes::BrushPreset> userPresets_;
        bool userPresetsScanned_ = false;

        // Collapse state for the four sections. Curated ("Presets ...")
        // ship closed: the artist already chose their starting point on
        // the toolbar, and the saved-presets drawer's primary purpose
        // is browsing the artist's own library. User sections ship open
        // so saved brushes are visible at a glance.
        bool presetsSharpExpanded_     = false;
        bool presetsTexturedExpanded_  = false;
        bool userSharpExpanded_        = true;
        bool userTexturedExpanded_     = true;

        void render_curated_section(HVYM::Brushes::BrushCategory cat);
        void render_user_section(HVYM::Brushes::BrushCategory cat);
        // Case-insensitive substring filter against searchQuery_.
        bool matches_filter(const std::string& name) const;
#endif
        Toolbar& toolbar_;
};
