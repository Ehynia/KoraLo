#pragma once
#include "project.hpp"
#include "selection_model.hpp"
#include "undo.hpp"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace koralo {

struct TimelineGeometry {
    int x = 180;
    int y = 520;
    int width = 1200;
    int height = 350;
    int ruler_h = 30;
    int header_w = 180;
    double px_per_sec = 90.0;
    double scroll_sec = 0.0;
    int scroll_y = 0;
};

struct TimelineVerticalScrollMetrics {
    int content_height = 0;
    int viewport_height = 0;
    int max_scroll = 0;
    int track_y = 0;
    int track_height = 0;
    int thumb_y = 0;
    int thumb_height = 0;
    int travel = 0;

    [[nodiscard]] bool visible() const { return max_scroll > 0 && track_height > 0; }
};

TimelineVerticalScrollMetrics timeline_vertical_scroll_metrics(
    const Project& project, const TimelineGeometry& geometry);
int timeline_vertical_scroll_from_thumb_top(
    const TimelineVerticalScrollMetrics& metrics, int thumb_top);

enum class DragMode {
    None,
    Move,
    TrimLeft,
    TrimRight,
    FadeIn,
    FadeOut,
    Slip,
    Roll,
    Slide,
    RippleTrim,
    BinMedia,
    Playhead,
    Marquee,
    TrackReorder
};

struct ClipVisualOverride {
    double start = 0.0;
    double duration = 0.0;
    double in_point = 0.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    int track_id = 0;
};


struct ClipClipboard {
    std::vector<Clip> clips;
    double origin = 0.0;
    [[nodiscard]] bool empty() const { return clips.empty(); }
    void clear() { clips.clear(); origin = 0.0; }
};

struct DragState {
    DragMode mode = DragMode::None;
    int clip_id = 0;
    int media_id = 0;
    int track_id = 0;
    int edge = 0;
    int origin_x = 0;
    int origin_y = 0;
    int current_x = 0;
    int current_y = 0;
    int source_track_index = -1;
    int target_track_index = -1;
    double anchor_time = 0.0;
    bool additive_selection = false;
    std::unordered_set<int> selection_base;
    std::unordered_set<int> primary_ids;
    std::unordered_set<int> left_neighbor_ids;
    std::unordered_set<int> right_neighbor_ids;
    std::unordered_set<int> ripple_ids;
    std::unordered_map<int, ClipVisualOverride> original;
    std::unordered_map<int, ClipVisualOverride> preview;
};

class EditController {
public:
    SelectionModel selection;
    int selected_media = 0;
    DragState drag;

    int hit_clip(const Project& p, const TimelineGeometry& g, int x, int y, int* edge = nullptr) const;
    int track_at(const Project& p, const TimelineGeometry& g, int y, TrackKind* kind = nullptr) const;
    double time_at(const TimelineGeometry& g, int x) const;
    int x_at(const TimelineGeometry& g, double time) const;

    void begin_clip_drag(const Project& p, const TimelineGeometry& g, int clip_id, int x, int y,
                         DragMode requested_mode = DragMode::None, bool add_selection = false);
    void begin_marquee(const Project& p, int x, int y, bool add_selection);
    void begin_track_reorder(const Project& p, const TimelineGeometry& g, int track_id, int x, int y);
    void begin_bin_drag(int media_id, int x, int y);
    void update_drag(const Project& p, const TimelineGeometry& g, int x, int y, double playhead);
    bool commit_drag(Project& p, UndoStack& undo, const TimelineGeometry& g, int x, int y, double playhead);
    void cancel_drag();

    bool split_selected(Project& p, UndoStack& undo, double playhead);
    bool delete_selected(Project& p, UndoStack& undo);
    bool nudge_selected(Project& p, UndoStack& undo, double delta_seconds);
    bool copy_selected(const Project& p, ClipClipboard& clipboard) const;
    bool cut_selected(Project& p, UndoStack& undo, ClipClipboard& clipboard);
    bool paste(Project& p, UndoStack& undo, const ClipClipboard& clipboard, double at_time);
    bool toggle_lock_selected(Project& p, UndoStack& undo);
    bool toggle_loop_selected(Project& p, UndoStack& undo);
    bool cycle_active_take(Project& p, UndoStack& undo, int direction = 1);
    bool trim_start_to_cursor(Project& p, UndoStack& undo, double playhead);
    bool trim_end_to_cursor(Project& p, UndoStack& undo, double playhead);
    bool insert_media(Project& p, UndoStack& undo, int media_id, double source_in, double source_out, double at_time);
    bool overwrite_media(Project& p, UndoStack& undo, int media_id, double source_in, double source_out, double at_time);

private:
    void update_marquee_selection(const Project& p, const TimelineGeometry& g);
    void update_track_reorder(const Project& p, const TimelineGeometry& g, int y);
    double snap_time(const Project& p, double candidate, double threshold_seconds,
                     const std::unordered_set<int>& ignore, double playhead) const;
};

} // namespace koralo
