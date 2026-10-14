#pragma once

#include "render_graph.hpp"

#include <optional>
#include <cstdint>
#include <vector>

namespace koralo {

// A conservative alternative to the general RGBA/black-background/overlay
// graph. Only a single unmodified video and an optional *matching* audio event
// can bypass the compositor; all edits, gaps, opacity and FX use RenderGraph.
struct DirectExportSource {
    const MediaItem* media = nullptr;
    const Clip* video = nullptr;
    bool include_audio = false;
};

std::optional<DirectExportSource> choose_direct_export(
    const Project& project, const RenderPlan& plan, const RenderGraph& graph);

// Pure-cut exports can avoid the RGBA overlay compositor entirely.  All video
// segments are frame-aligned, opaque, sequential, on one track and free of FX;
// audio retains the regular (independent) audio mix graph.
struct CutExportSegment {
    const Clip* clip = nullptr;
    const MediaItem* media = nullptr;
    std::uint64_t frames = 0;
};

std::optional<std::vector<CutExportSegment>> choose_cut_export(
    const Project& project, const RenderPlan& plan, const RenderGraph& graph);

}  // namespace koralo
