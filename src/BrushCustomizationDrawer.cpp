#include "BrushCustomizationDrawer.hpp"

#include "Toolbar.hpp"
#include "MainProgram.hpp"
#include "World.hpp"
#include "DrawingProgram/DrawingProgram.hpp"
#include "DrawingProgram/Tools/MyPaintBrushTool.hpp"
#include "GUIStuff/GUIManager.hpp"
#include "GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextBoxHelpers.hpp"

#ifdef HVYM_HAS_LIBMYPAINT
#include "Brushes/BrushPresets.hpp"
#include "Brushes/MyPaintBrushParams.hpp"
extern "C" {
#include <mypaint-brush.h>
}
#endif

#include <string>
#include <vector>

BrushCustomizationDrawer::BrushCustomizationDrawer(Toolbar& toolbar):
    toolbar_(toolbar)
{
#ifdef HVYM_HAS_LIBMYPAINT
    // Default expanded state: Basic open, everything else collapsed.
    // Users who tune Smudge / Speed / etc. will expand those once and
    // the per-drawer state persists across opens (this instance is
    // owned by Toolbar, not constructed per popup).
    groupExpanded_[static_cast<size_t>(HVYM::Brushes::BrushParamGroup::Basic)] = true;
#endif
}

void BrushCustomizationDrawer::render_body() {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& main = toolbar_.main_program();
    auto& gui  = main.g.gui;
    if (!main.world) return;

    auto& drawTool = main.world->drawProg.drawTool;
    if (!drawTool || drawTool->get_type() != DrawingProgramToolType::MYPAINTBRUSH) {
        // Active tool is not the MyPaint brush. The icon button gating
        // in Toolbar.cpp already prevents this from being reachable in
        // the common case, but a tool switch could theoretically race
        // an open drawer -- bail rather than dereference nullptr.
        GUIStuff::ElementHelpers::text_label(gui, "Switch to a MyPaint brush to customize.");
        return;
    }

    auto* brushTool = static_cast<MyPaintBrushTool*>(drawTool.get());
    MyPaintBrush* brush = brushTool->get_brush();
    if (!brush) return;

    // Snapshot live values from the brush so sliders show the current
    // state (after a preset apply, or after a prior slider edit). Cheap;
    // 65 base-value reads per frame the drawer is open.
    for (int i = 0; i < MYPAINT_BRUSH_SETTINGS_COUNT; ++i) {
        liveValues_[i] = mypaint_brush_get_base_value(brush,
                            static_cast<MyPaintBrushSetting>(i));
    }

    using namespace GUIStuff;
    using namespace ElementHelpers;

    text_label_centered(gui, "Brush Customization");

    // Active preset name -- read-only label for now. A1.M5 turns this
    // into a Save / Save-as button cluster.
    auto& cfg = main.toolConfig.myPaintBrush;
    const auto& presets = HVYM::Brushes::curated_presets();
    if (!presets.empty() && cfg.activePresetIndex >= 0
        && cfg.activePresetIndex < static_cast<int>(presets.size())) {
        text_label(gui, std::string("Active preset: ") + presets[cfg.activePresetIndex].name);
    }

    // Group the params by BrushParamGroup. Render each group as an
    // expandable section: header is a clickable button, body (the slider
    // rows) renders only when the group is expanded. This keeps the
    // active widget count low on idle frames -- 57 always-rendered
    // slider+textbox pairs in one ScrollArea triggered render
    // inconsistencies; with most groups collapsed the visible widget
    // count drops to single digits and the popup behaves cleanly.
    //
    // Two-pass layout: bucket the params into per-group lists first so
    // each group's header + body render in one contiguous Clay scope
    // (the table's libmypaint-enum order has a few group stragglers --
    // e.g. RESTORE_COLOR/Smudge in the middle of TRACKING, COLORIZE/
    // Rendering after ELLIPTICAL/Shape -- so naive group-on-transition
    // would split groups into multiple sections).
    constexpr size_t kGroupCount = 16;
    std::array<std::vector<const HVYM::Brushes::BrushParamMeta*>, kGroupCount> byGroup{};
    for (const auto& meta : HVYM::Brushes::all_params()) {
        byGroup[static_cast<size_t>(meta.group)].push_back(&meta);
    }

    // Iterate the BrushParamGroup enum in declaration order so the
    // drawer layout is stable run-to-run, regardless of how params
    // happen to be ordered inside libmypaint's brushsettings.json.
    constexpr HVYM::Brushes::BrushParamGroup kGroupOrder[] = {
        HVYM::Brushes::BrushParamGroup::Basic,
        HVYM::Brushes::BrushParamGroup::Dabs,
        HVYM::Brushes::BrushParamGroup::Speed,
        HVYM::Brushes::BrushParamGroup::Smudge,
        HVYM::Brushes::BrushParamGroup::Jitter,
        HVYM::Brushes::BrushParamGroup::Tracking,
        HVYM::Brushes::BrushParamGroup::Shape,
        HVYM::Brushes::BrushParamGroup::Stroke,
        HVYM::Brushes::BrushParamGroup::GridMap,
        HVYM::Brushes::BrushParamGroup::Rendering,
        HVYM::Brushes::BrushParamGroup::Custom,
    };

    // Element-identity gotcha: GUIManagerID(const char*) stores the
    // *pointer* (not a string copy) as its hash key. C-string IDs MUST
    // be stable across frames -- either string literals or libmypaint
    // statics. Per-frame std::string::c_str() produces a fresh pointer
    // each render and would make every header / slider a brand-new
    // element with no click-tracking history. We use gui.new_id(int64_t)
    // here so the per-iteration uniqueness comes from a value-based
    // hash, and the inner widgets use stable string literals or
    // libmypaint static cname pointers.
    for (auto group : kGroupOrder) {
        const auto gi = static_cast<size_t>(group);
        const auto& members = byGroup[gi];
        if (members.empty()) continue;

        const std::string groupName(HVYM::Brushes::group_display_name(group));
        const std::string headerLabel = (groupExpanded_[gi] ? "- " : "+ ") + groupName;

        // numeric ID = group enum value, offset so it can't collide with
        // a setting ID below (settings are 0..64; groups are 1000+).
        gui.new_id(static_cast<int64_t>(1000 + gi), [&] {
            text_button(gui, "group header", headerLabel, TextButtonOptions{
                .wide    = true,
                .onClick = [this, gi, &gui]() {
                    groupExpanded_[gi] = !groupExpanded_[gi];
                    gui.set_to_layout();
                },
            });
        });

        if (!groupExpanded_[gi]) continue;

        for (const auto* metaPtr : members) {
            const auto& meta = *metaPtr;
            const auto id    = meta.id;
            const auto range = HVYM::Brushes::param_range(id);
            // libmypaint's display name pointer is stable static memory
            // (lives in the autogenerated mypaint-brush-settings.c
            // table); we copy it into a std::string for the label only
            // because slider_scalar_field takes std::string_view, but
            // we never use it as an ID.
            const auto label = std::string(HVYM::Brushes::param_display_name(id));

            TextBoxScalarOptions opts;
            opts.onEdit = [brush, id, slot = &liveValues_[id]]() {
                mypaint_brush_set_base_value(brush, id, *slot);
            };
            // Numeric ID = the libmypaint setting enum value, which is
            // globally unique across all 65 settings and stable forever.
            gui.new_id(static_cast<int64_t>(id), [&] {
                slider_scalar_field<float>(gui, "slider", label,
                                           &liveValues_[id],
                                           range.min, range.max, opts);
            });
        }
    }
#endif  // HVYM_HAS_LIBMYPAINT
}
