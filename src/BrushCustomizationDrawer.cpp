#include "BrushCustomizationDrawer.hpp"

#include "Toolbar.hpp"
#include "MainProgram.hpp"
#include "World.hpp"
#include "DrawingProgram/DrawingProgram.hpp"
#include "DrawingProgram/Tools/MyPaintBrushTool.hpp"
#include "GUIStuff/GUIManager.hpp"
#include "GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include "GUIStuff/ElementHelpers/TextBoxHelpers.hpp"

#ifdef HVYM_HAS_LIBMYPAINT
#include "Brushes/BrushPresets.hpp"
#include "Brushes/MyPaintBrushParams.hpp"
#include "Brushes/UserBrushPresets.hpp"
#include "DrawingProgram/Tools/SquareCanvasCaptureTool.hpp"
extern "C" {
#include <mypaint-brush.h>
}
#endif

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkPngEncoder.h>
#include <cstring>

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

    // When the save-as modal is open we replace the params view with
    // the modal body so the popup stays the same size and the modal
    // is unambiguously the focus.
    if (saveModalOpen_) {
        render_save_modal();
        return;
    }

    text_label_centered(gui, "Brush Customization");

    // Active preset name + "Save as preset..." button. The button
    // snapshots the live brush state (base values + pressure curves)
    // into a BrushParams via brush_params_from_live, then opens a
    // modal for the artist to pick a name + category before writing
    // to <configPath>/brush_presets/<category>/<slug>.json via
    // UserBrushPresets::save.
    auto& cfg = main.toolConfig.myPaintBrush;
    const auto& presets = HVYM::Brushes::curated_presets();
    if (!presets.empty() && cfg.activePresetIndex >= 0
        && cfg.activePresetIndex < static_cast<int>(presets.size())) {
        text_label(gui, std::string("Based on: ") + presets[cfg.activePresetIndex].name);
        // Seed the save-as category from the active curated preset's
        // category so the artist's saved tweak lands in the same bucket
        // by default.
        text_button(gui, "save as preset", "Save as preset...", TextButtonOptions{
            .wide    = true,
            .onClick = [this, &cfg, &presets, &gui]() {
                if (cfg.activePresetIndex >= 0
                    && cfg.activePresetIndex < static_cast<int>(presets.size())) {
                    saveModalCategory_ = presets[cfg.activePresetIndex].category;
                    saveModalName_     = std::string("My ") + presets[cfg.activePresetIndex].name;
                }
                saveModalOpen_ = true;
                gui.set_to_layout();
            },
        });
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

void BrushCustomizationDrawer::render_save_modal() {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& main = toolbar_.main_program();
    auto& gui  = main.g.gui;
    if (!main.world) return;
    auto& drawTool = main.world->drawProg.drawTool;
    if (!drawTool || drawTool->get_type() != DrawingProgramToolType::MYPAINTBRUSH) {
        // Tool switched away mid-modal -- just close.
        saveModalOpen_ = false;
        return;
    }
    auto* brushTool = static_cast<MyPaintBrushTool*>(drawTool.get());
    MyPaintBrush* brush = brushTool->get_brush();
    if (!brush) return;

    using namespace GUIStuff;
    using namespace ElementHelpers;

    text_label_centered(gui, "Save brush preset");
    input_text_field(gui, "save preset name", "Name", &saveModalName_);

    text_label(gui, "Category:");
    radio_button_selector<HVYM::Brushes::BrushCategory>(
        gui, "save preset category", &saveModalCategory_,
        {
            {"Sharp",    HVYM::Brushes::BrushCategory::SHARP},
            {"Textured", HVYM::Brushes::BrushCategory::TEXTURED},
        });

    // Icon section. capturedIcon_ is set by the SquareCanvasCaptureTool
    // callback after the artist drags a square on the canvas. No inline
    // preview yet (no SkImage-from-memory display widget in tree); A2
    // drawer renders the on-disk icon once it's saved.
    if (capturedIcon_) {
        text_label(gui, "Icon: 64x64 captured");
        text_button(gui, "save preset clear icon", "Clear icon", TextButtonOptions{
            .wide    = true,
            .onClick = [this, &gui]() {
                capturedIcon_ = nullptr;
                gui.set_to_layout();
            },
        });
    } else {
        text_label(gui, "Icon: (none)");
    }
    text_button(gui, "save preset capture icon", "Capture icon...", TextButtonOptions{
        .wide    = true,
        .onClick = [this]() { start_icon_capture(); },
    });

    text_button(gui, "save preset confirm", "Save", TextButtonOptions{
        .wide    = true,
        .onClick = [this, brush, &main, &gui]() {
            if (saveModalName_.empty()) return;  // require a name; click is a no-op
            HVYM::Brushes::BrushPreset preset;
            preset.name     = saveModalName_;
            preset.category = saveModalCategory_;
            preset.params   = HVYM::Brushes::brush_params_from_live(brush);

            // PNG-encode the captured icon (Waypoint::encode_skin_png
            // shape). std::nullopt when no icon: UserBrushPresets::save
            // removes any pre-existing sidecar so the identicon
            // fallback takes over.
            std::optional<std::vector<uint8_t>> iconBytes;
            if (capturedIcon_) {
                sk_sp<SkImage> raster = capturedIcon_->makeRasterImage(nullptr);
                SkPixmap pix;
                if (raster && raster->peekPixels(&pix)) {
                    SkDynamicMemoryWStream stream;
                    if (SkPngEncoder::Encode(&stream, pix, {})) {
                        auto data = stream.detachAsData();
                        if (data) {
                            std::vector<uint8_t> bytes(data->size());
                            std::memcpy(bytes.data(), data->bytes(), data->size());
                            iconBytes = std::move(bytes);
                        }
                    }
                }
            }
            UserBrushPresets::save(main.conf.configPath, preset, iconBytes);
            saveModalOpen_ = false;
            saveModalName_.clear();
            capturedIcon_ = nullptr;
            gui.set_to_layout();
        },
    });
    text_button(gui, "save preset cancel", "Cancel", TextButtonOptions{
        .wide    = true,
        .onClick = [this, &gui]() {
            saveModalOpen_ = false;
            saveModalName_.clear();
            capturedIcon_ = nullptr;
            gui.set_to_layout();
        },
    });
#endif
}

void BrushCustomizationDrawer::start_icon_capture() {
#ifdef HVYM_HAS_LIBMYPAINT
    auto& main = toolbar_.main_program();
    if (!main.world) return;
    auto& drawP = main.world->drawProg;
    const auto previousToolType = drawP.drawTool ? drawP.drawTool->get_type()
                                                 : DrawingProgramToolType::MYPAINTBRUSH;

    // The capture tool needs an unobstructed canvas, so close the
    // drawer popup before activating it. The save modal flag stays
    // true so the capture callback re-opens the popup directly into
    // the modal (rather than dumping the artist back to the params
    // view, where they'd have to click Save as preset... again).
    toolbar_.set_brush_customization_menu_open(false);

    auto onCapture = [this](sk_sp<SkImage> image) {
        capturedIcon_ = std::move(image);
        // Re-open the drawer popup. saveModalOpen_ was preserved, so
        // render_body sees it true and routes straight back to the
        // save-modal view with the icon now attached.
        toolbar_.set_brush_customization_menu_open(true);
    };

    auto tool = std::make_unique<SquareCanvasCaptureTool>(
        drawP, /*targetSize=*/64, previousToolType, std::move(onCapture));
    drawP.switch_to_tool_ptr(std::move(tool));
#endif
}
