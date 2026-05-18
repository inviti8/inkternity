#include "SavedPresetsDrawer.hpp"

#include "Toolbar.hpp"
#include "MainProgram.hpp"
#include "World.hpp"
#include "DrawingProgram/DrawingProgram.hpp"
#include "DrawingProgram/Tools/MyPaintBrushTool.hpp"
#include "GUIStuff/GUIManager.hpp"
#include "GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "GUIStuff/Elements/ImageDisplay.hpp"

#ifdef HVYM_HAS_LIBMYPAINT
#include "Brushes/BrushPresets.hpp"
#include "Brushes/UserBrushPresets.hpp"
extern "C" {
#include <mypaint-brush.h>
}
#endif

#include <algorithm>
#include <cctype>

SavedPresetsDrawer::SavedPresetsDrawer(Toolbar& toolbar):
    toolbar_(toolbar)
{}

#ifdef HVYM_HAS_LIBMYPAINT

bool SavedPresetsDrawer::matches_filter(const std::string& name) const {
    if (searchQuery_.empty()) return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const auto needle = lower(searchQuery_);
    return lower(name).find(needle) != std::string::npos;
}

void SavedPresetsDrawer::render_category(HVYM::Brushes::BrushCategory cat) {
    using namespace GUIStuff;
    using namespace ElementHelpers;
    auto& main = toolbar_.main_program();
    if (!main.world) return;
    auto& gui = main.g.gui;
    auto& drawTool = main.world->drawProg.drawTool;
    if (!drawTool || drawTool->get_type() != DrawingProgramToolType::MYPAINTBRUSH) return;
    auto* brushTool = static_cast<MyPaintBrushTool*>(drawTool.get());
    MyPaintBrush* brush = brushTool->get_brush();
    auto& cfg = main.toolConfig.myPaintBrush;

    // Collect everything in this category that passes the filter so we
    // can decide whether to render the header at all (no header for an
    // empty section -- visual noise reduction during search).
    const auto& curated = HVYM::Brushes::curated_presets();
    std::vector<int> curatedIdxs;
    for (int i = 0; i < static_cast<int>(curated.size()); ++i) {
        if (curated[i].category == cat && matches_filter(curated[i].name))
            curatedIdxs.push_back(i);
    }
    std::vector<const HVYM::Brushes::BrushPreset*> userInCat;
    for (const auto& p : userPresets_) {
        if (p.category == cat && matches_filter(p.name))
            userInCat.push_back(&p);
    }
    if (curatedIdxs.empty() && userInCat.empty()) return;

    text_label_centered(gui, cat == HVYM::Brushes::BrushCategory::SHARP ? "Sharp" : "Textured");

    // Numeric IDs disambiguate tiles across renders. Curated tiles use
    // 1..N (their index + 1); user tiles use 1000 + N. See the matching
    // pattern in BrushCustomizationDrawer for why string-IDs aren't
    // safe here.
    for (int i : curatedIdxs) {
        const auto& preset = curated[i];
        const bool isActive = cfg.activeUserPresetPath.empty() && cfg.activePresetIndex == i;
        gui.new_id(static_cast<int64_t>(1 + i), [&] {
            text_button(gui, "tile", preset.name, TextButtonOptions{
                .isSelected = isActive,
                .wide       = true,
                .onClick    = [brush, &cfg, i, &gui] {
                    cfg.activePresetIndex     = i;
                    cfg.activeUserPresetPath.clear();
                    HVYM::Brushes::apply_preset(brush, i);
                    const auto& ov = cfg.overrides[i];
                    HVYM::Brushes::apply_tunable_overrides(brush, ov.diameter, ov.hardness, ov.opacity);
                    gui.set_to_layout();
                },
            });
        });
    }
    for (const auto* presetPtr : userInCat) {
        const auto& preset = *presetPtr;
        // Use the json sibling path as the persistent identifier.
        const auto jsonPath = UserBrushPresets::preset_json_path(
            main.conf.configPath, preset.category, preset.name).string();
        const bool isActive = cfg.activeUserPresetPath == jsonPath;
        // Stable numeric ID derived from a hash of the path so tile
        // identity survives across renders even as the user-preset
        // list grows or shrinks.
        const int64_t tileId = 1000 +
            static_cast<int64_t>(std::hash<std::string>{}(jsonPath) & 0x7fffffff);
        gui.new_id(tileId, [&] {
            // Horizontal row: icon thumbnail (when sidecar PNG exists) +
            // name button + small delete button. Delete is the only
            // context-menu action shipped in v1; Rename, Edit, Duplicate
            // are deferred to A2.M3-extended.
            CLAY_AUTO_ID({
                .layout = {
                    .sizing          = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                    .childGap        = 4,
                    .childAlignment  = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                // Captured icon thumbnail (PHASE3.md §3 A1.M6 sidecar
                // PNG). ImageDisplay decodes from the filesystem path
                // each layout, which is fine for icons -- they don't
                // change in place the way the avatar does (preset re-
                // capture writes a different slug). Skipped when the
                // preset has no sidecar so the tile collapses to text.
                if (!preset.iconPath.empty()) {
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {.width = CLAY_SIZING_FIXED(28), .height = CLAY_SIZING_FIXED(28)}
                        }
                    }) {
                        gui.element<ImageDisplay>("icon", ImageDisplay::Data{
                            .imgPath = preset.iconPath,
                            .radius  = 4.0f
                        });
                    }
                }
                CLAY_AUTO_ID({
                    .layout = {
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}
                    }
                }) {
                    text_button(gui, "tile", preset.name, TextButtonOptions{
                        .isSelected = isActive,
                        .wide       = true,
                        .onClick    = [brush, &cfg, jsonPath, &gui] {
                            cfg.activeUserPresetPath = jsonPath;
                            // Apply once immediately for instant feedback;
                            // begin_stroke re-applies via the same path so
                            // the selection sticks across strokes.
                            if (auto params = UserBrushPresets::read_params_json(jsonPath))
                                HVYM::Brushes::apply_brush_params(brush, *params);
                            gui.set_to_layout();
                        },
                    });
                }
                // Capture the path + cat + name by value -- the underlying
                // userPresets_ list gets rebuilt on next render, so the
                // BrushPreset* would dangle by the time the onClick fires.
                const auto categoryCopy = preset.category;
                const auto nameCopy     = preset.name;
                text_button(gui, "del", "X", TextButtonOptions{
                    .onClick = [this, &cfg, jsonPath, categoryCopy, nameCopy, &gui]() {
                        UserBrushPresets::remove(toolbar_.main_program().conf.configPath,
                                                 categoryCopy, nameCopy);
                        // If the deleted preset was active, drop the
                        // reference so apply_active falls back to the
                        // curated path on next stroke.
                        if (cfg.activeUserPresetPath == jsonPath)
                            cfg.activeUserPresetPath.clear();
                        gui.set_to_layout();
                    }
                });
            }
        });
    }
}

#endif  // HVYM_HAS_LIBMYPAINT

void SavedPresetsDrawer::render_body() {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& main = toolbar_.main_program();
    auto& gui  = main.g.gui;
    if (!main.world) return;

    auto& drawTool = main.world->drawProg.drawTool;
    if (!drawTool || drawTool->get_type() != DrawingProgramToolType::MYPAINTBRUSH) {
        GUIStuff::ElementHelpers::text_label(gui, "Switch to a Pixel brush to pick a preset.");
        return;
    }

    // Re-scan disk on every render so newly-saved presets (from the
    // customization drawer's Save flow) show up without needing to
    // close + reopen this drawer. User-preset directory listings are
    // small; cost is negligible.
    userPresets_ = UserBrushPresets::scan(main.conf.configPath);
    userPresetsScanned_ = true;

    using namespace GUIStuff;
    using namespace ElementHelpers;

    text_label_centered(gui, "Saved Presets");
    input_text_field(gui, "saved presets search", "Search", &searchQuery_);

    render_category(HVYM::Brushes::BrushCategory::SHARP);
    render_category(HVYM::Brushes::BrushCategory::TEXTURED);
#endif
}
