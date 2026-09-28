#pragma once
// ANIMATED_IMPORTS.md Phase 3 — rasterize a layer group's child layers and write a
// layered PSD (via PsdWriter), so a character built as a group in Inkternity can be
// rigged/animated externally (SkelForm imports PSD). Each leaf layer becomes one PSD
// layer at a shared document scale; layer alpha + visibility carry into the PSD.
//
// MUST run on the main/GL thread — it renders through the GPU surface
// (MainProgram::create_native_surface). The menu path defers to it via
// DrawingProgram::process_pending_psd_export().

#include <filesystem>

class DrawingProgram;
class DrawingProgramLayerListItem;

namespace PsdExport {

// Rasterize `group` (a folder, or a single layer) and write a .psd to `path`.
// Returns true on success; logs USERINFO/WORLDFATAL either way.
bool export_group(DrawingProgram& drawP, DrawingProgramLayerListItem& group,
                  const std::filesystem::path& path);

}  // namespace PsdExport
