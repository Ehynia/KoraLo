#include "edit_controller.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <unordered_map>

namespace koralo {

TimelineVerticalScrollMetrics timeline_vertical_scroll_metrics(
    const Project& project, const TimelineGeometry& geometry) {
    TimelineVerticalScrollMetrics metrics;
    for (const auto& track : project.tracks) metrics.content_height += track.height;

    metrics.viewport_height = std::max(0, geometry.height - geometry.ruler_h);
    metrics.max_scroll = std::max(0, metrics.content_height - metrics.viewport_height);
    metrics.track_y = geometry.y + geometry.ruler_h + 2;
    metrics.track_height = std::max(0, metrics.viewport_height - 4);
    if (!metrics.visible()) {
        metrics.thumb_y = metrics.track_y;
        metrics.thumb_height = metrics.track_height;
        return metrics;
    }

    const double visible_fraction = std::clamp(
        metrics.viewport_height / static_cast<double>(std::max(1, metrics.content_height)), 0.0, 1.0);
    const int minimum_thumb = std::min(20, metrics.track_height);
    metrics.thumb_height = std::clamp(
        static_cast<int>(std::lround(metrics.track_height * visible_fraction)),
        minimum_thumb, metrics.track_height);
    metrics.travel = std::max(0, metrics.track_height - metrics.thumb_height);
    const int clamped_scroll = std::clamp(geometry.scroll_y, 0, metrics.max_scroll);
    metrics.thumb_y = metrics.track_y + (metrics.travel > 0
        ? static_cast<int>(std::lround(metrics.travel * (clamped_scroll / static_cast<double>(metrics.max_scroll))))
        : 0);
    return metrics;
}

int timeline_vertical_scroll_from_thumb_top(
    const TimelineVerticalScrollMetrics& metrics, int thumb_top) {
    if (!metrics.visible() || metrics.travel <= 0) return 0;
    const int local_top = std::clamp(thumb_top - metrics.track_y, 0, metrics.travel);
    return static_cast<int>(std::lround(
        metrics.max_scroll * (local_top / static_cast<double>(metrics.travel))));
}

namespace {

std::unordered_set<int> group_ids(const Project& p, const Clip* root) {
    std::unordered_set<int> ids;
    if (!root) return ids;
    if (!p.ignore_event_grouping && root->group_id > 0) {
        for (const Clip* c : p.clips_in_group(root->group_id)) ids.insert(c->id);
    } else {
        ids.insert(root->id);
    }
    return ids;
}

const Clip* touching_previous(const Project& p, const Clip& clip, double epsilon) {
    const Clip* best = nullptr;
    for (const Clip* candidate : p.clips_on_track(clip.track_id)) {
        if (candidate->id == clip.id) continue;
        if (std::abs(candidate->end() - clip.start) <= epsilon) {
            if (!best || candidate->start > best->start) best = candidate;
        }
    }
    return best;
}

const Clip* touching_next(const Project& p, const Clip& clip, double epsilon) {
    const Clip* best = nullptr;
    for (const Clip* candidate : p.clips_on_track(clip.track_id)) {
        if (candidate->id == clip.id) continue;
        if (std::abs(candidate->start - clip.end()) <= epsilon) {
            if (!best || candidate->start < best->start) best = candidate;
        }
    }
    return best;
}

void capture_clip(const Project& p, DragState& drag, int id) {
    if (drag.original.contains(id)) return;
    const Clip* c = p.clip_by_id(id);
    if (!c) return;
    ClipVisualOverride v{c->start, c->duration, c->source_offset(), c->fade_in, c->fade_out, c->track_id};
    drag.original[id] = v;
    drag.preview[id] = v;
}

bool editable_clip(const Project& p, const Clip* c) {
    if (!c || c->locked) return false;
    const Track* track = p.track_by_id(c->track_id);
    return track && !track->locked;
}

bool all_editable(const Project& p, const std::unordered_set<int>& ids) {
    for (int id : ids) if (!editable_clip(p, p.clip_by_id(id))) return false;
    return true;
}

void copy_clip_properties(const Clip& source, Clip& target) {
    target.rate = source.rate;
    target.gain = source.gain;
    target.opacity = source.opacity;
    target.fade_in = source.fade_in;
    target.fade_out = source.fade_out;
    target.snap_offset = source.snap_offset;
    target.muted = source.muted;
    target.locked = false;
    target.loop = source.loop;
    target.takes = source.takes;
    target.active_take = source.active_take;
}

double min_source_shift(const Clip& clip, bool all_takes) {
    const double rate = std::max(0.0001, clip.rate);
    if (clip.takes.empty()) return 0.0;
    double minimum = -std::numeric_limits<double>::infinity();
    if (!all_takes) {
        if (const Take* take = clip.active_take_ptr()) return -take->offset / rate;
        return 0.0;
    }
    for (const auto& take : clip.takes) minimum = std::max(minimum, -take.offset / rate);
    return minimum;
}

double max_source_extension(const Project& p, const Clip& clip, double source_offset, bool all_takes) {
    if (clip.loop) return std::numeric_limits<double>::infinity();
    const double rate = std::max(0.0001, clip.rate);
    double maximum = std::numeric_limits<double>::infinity();
    if (!all_takes) {
        const Take* take = clip.active_take_ptr();
        if (!take) return 0.0;
        const MediaItem* media = p.media_by_id(take->media_id);
        if (!media || media->duration <= 0.0) return maximum;
        if (is_still_image_path(media->path)) return maximum;
        return (media->duration - source_offset) / rate - clip.duration;
    }
    for (const auto& take : clip.takes) {
        const MediaItem* media = p.media_by_id(take.media_id);
        if (!media || media->duration <= 0.0) continue;
        if (is_still_image_path(media->path)) continue;
        const double shifted_offset = take.offset + (source_offset - clip.source_offset());
        maximum = std::min(maximum, (media->duration - shifted_offset) / rate - clip.duration);
    }
    return maximum;
}

void shift_take_offsets(Clip& clip, double source_delta, bool all_takes) {
    if (all_takes) {
        for (auto& take : clip.takes) take.offset = std::max(0.0, take.offset + source_delta);
    } else if (Take* take = clip.active_take_ptr()) {
        take->offset = std::max(0.0, take->offset + source_delta);
    }
}

std::vector<int> destination_tracks(const Project& p, const MediaItem& media) {
    std::vector<int> tracks;
    if (media.has_video) {
        for (const auto& track : p.tracks) if (track.kind == TrackKind::Video && !track.locked) { tracks.push_back(track.id); break; }
    }
    if (media.has_audio) {
        for (const auto& track : p.tracks) if (track.kind == TrackKind::Audio && !track.locked) { tracks.push_back(track.id); break; }
    }
    return tracks;
}

int kind_track_position(const Project& p, int track_id) {
    const Track* target = p.track_by_id(track_id);
    if (!target) return -1;
    int position = 0;
    for (const auto& track : p.tracks) {
        if (track.kind != target->kind) continue;
        if (track.id == track_id) return position;
        ++position;
    }
    return -1;
}

const Track* kind_track_at(const Project& p, TrackKind kind, int position) {
    if (position < 0) return nullptr;
    int current = 0;
    for (const auto& track : p.tracks) {
        if (track.kind != kind) continue;
        if (current++ == position) return &track;
    }
    return nullptr;
}

double quantize_time(const Project& p, double time) {
    if (!p.quantize_to_frames) return std::max(0.0, time);
    const double fps = std::max(1.0, p.settings.fps);
    return std::max(0.0, std::round(time * fps) / fps);
}

} // namespace

int EditController::x_at(const TimelineGeometry& g, double time) const {
    return g.x + g.header_w + static_cast<int>(std::llround((time - g.scroll_sec) * g.px_per_sec));
}

double EditController::time_at(const TimelineGeometry& g, int x) const {
    return std::clamp(g.scroll_sec + (x - g.x - g.header_w) / std::max(1.0, g.px_per_sec), 0.0, kTimelineMaxSeconds);
}

int EditController::track_at(const Project& p, const TimelineGeometry& g, int y, TrackKind* kind) const {
    int yy = g.y + g.ruler_h - g.scroll_y;
    for (const auto& t : p.tracks) {
        if (y >= yy && y < yy + t.height) {
            if (kind) *kind = t.kind;
            return t.id;
        }
        yy += t.height;
    }
    return 0;
}

int EditController::hit_clip(const Project& p, const TimelineGeometry& g, int x, int y, int* edge) const {
    // Event geometry is allowed to extend beyond the visible time window when
    // the timeline is horizontally scrolled, but the hidden portion must never
    // become interactive through the fixed track-header pane.
    const int content_left = g.x + g.header_w;
    const int content_right = g.x + g.width;
    if (x < content_left || x >= content_right) return 0;
    int tid = track_at(p, g, y);
    if (!tid) return 0;
    int yy = g.y + g.ruler_h - g.scroll_y;
    for (const auto& t : p.tracks) {
        if (t.id == tid) break;
        yy += t.height;
    }
    for (auto it = p.clips.rbegin(); it != p.clips.rend(); ++it) {
        if (it->track_id != tid) continue;
        int x1 = x_at(g, it->start);
        int x2 = x_at(g, it->end());
        if (x >= x1 && x <= x2 && y >= yy + 5 && y <= yy + p.track_by_id(tid)->height - 5) {
            if (edge) {
                constexpr int edge_px = 7;
                *edge = (std::abs(x - x1) <= edge_px ? -1 : (std::abs(x - x2) <= edge_px ? 1 : 0));
            }
            return it->id;
        }
    }
    return 0;
}

void EditController::begin_clip_drag(const Project& p, const TimelineGeometry& g, int clip_id, int x, int y,
                                     DragMode requested_mode, bool add_selection) {
    const Clip* root = p.clip_by_id(clip_id);
    if (!root) return;
    if (!editable_clip(p, root)) {
        selection.select_clip(p, clip_id, add_selection ? SelectionOperation::Add : SelectionOperation::Replace, true);
        drag = {};
        return;
    }
    constexpr int edge_px = 7;
    const int start_x = x_at(g, root->start);
    const int end_x = x_at(g, root->end());
    const int edge = std::abs(x - start_x) <= edge_px ? -1 : (std::abs(x - end_x) <= edge_px ? 1 : 0);
    const bool preserve_existing = selection.contains(clip_id) && !add_selection;
    if (!preserve_existing) {
        selection.select_clip(p, clip_id, add_selection ? SelectionOperation::Add : SelectionOperation::Replace, true);
    }

    drag = {};
    drag.clip_id = clip_id;
    drag.edge = edge;
    drag.origin_x = x;
    drag.origin_y = y;
    drag.current_x = x;
    drag.current_y = y;

    drag.anchor_time = edge < 0 ? root->start : (edge > 0 ? root->end() : root->start);

    DragMode mode = requested_mode;
    if (mode == DragMode::None) mode = edge < 0 ? DragMode::TrimLeft : (edge > 0 ? DragMode::TrimRight : DragMode::Move);
    if (mode == DragMode::Roll && edge == 0) mode = DragMode::Move;
    if (mode == DragMode::RippleTrim && edge == 0) mode = DragMode::Move;
    if (mode == DragMode::Slide && edge != 0) mode = edge < 0 ? DragMode::TrimLeft : DragMode::TrimRight;
    drag.mode = mode;

    if (mode == DragMode::FadeIn || mode == DragMode::FadeOut) {
        // VEGAS-style fade handles are per-event even when A/V events are
        // grouped. Editing a fade must never split or rewrite the group.
        drag.primary_ids = {root->id};
        capture_clip(p, drag, root->id);
        return;
    }

    const double epsilon = 0.5 / std::max(1.0, p.settings.fps);
    if (mode == DragMode::Move) {
        drag.primary_ids = selection.clips();
        if (!all_editable(p, drag.primary_ids)) { drag = {}; return; }
        for (int id : drag.primary_ids) capture_clip(p, drag, id);
        return;
    }

    drag.primary_ids = group_ids(p, root);
    if (!all_editable(p, drag.primary_ids)) { drag = {}; return; }

    if (mode == DragMode::Roll) {
        const Clip* left = edge > 0 ? root : touching_previous(p, *root, epsilon);
        const Clip* right = edge > 0 ? touching_next(p, *root, epsilon) : root;
        if (!left || !right) {
            drag.mode = edge < 0 ? DragMode::TrimLeft : DragMode::TrimRight;
            for (int id : drag.primary_ids) capture_clip(p, drag, id);
            return;
        }
        drag.left_neighbor_ids = group_ids(p, left);
        drag.right_neighbor_ids = group_ids(p, right);
        drag.anchor_time = left->end();
        for (int id : drag.left_neighbor_ids) capture_clip(p, drag, id);
        for (int id : drag.right_neighbor_ids) capture_clip(p, drag, id);
        return;
    }

    if (mode == DragMode::Slide) {
        bool valid = true;
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            const Clip* left = touching_previous(p, *c, epsilon);
            const Clip* right = touching_next(p, *c, epsilon);
            if (!left || !right) {
                valid = false;
                break;
            }
            drag.left_neighbor_ids.insert(left->id);
            drag.right_neighbor_ids.insert(right->id);
        }
        if (!valid) {
            drag.mode = DragMode::Move;
            drag.primary_ids = selection.clips();
            for (int id : drag.primary_ids) capture_clip(p, drag, id);
            return;
        }
        for (int id : drag.primary_ids) capture_clip(p, drag, id);
        for (int id : drag.left_neighbor_ids) capture_clip(p, drag, id);
        for (int id : drag.right_neighbor_ids) capture_clip(p, drag, id);
        return;
    }

    if (mode == DragMode::RippleTrim) {
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            for (const Clip* candidate : p.clips_on_track(c->track_id)) {
                if (drag.primary_ids.contains(candidate->id)) continue;
                if (candidate->start + epsilon >= c->end()) drag.ripple_ids.insert(candidate->id);
            }
        }
        for (int id : drag.primary_ids) capture_clip(p, drag, id);
        for (int id : drag.ripple_ids) capture_clip(p, drag, id);
        return;
    }

    for (int id : drag.primary_ids) capture_clip(p, drag, id);
}

void EditController::begin_marquee(const Project& p, int x, int y, bool add_selection) {
    drag = {};
    drag.mode = DragMode::Marquee;
    drag.origin_x = x;
    drag.origin_y = y;
    drag.current_x = x;
    drag.current_y = y;
    drag.additive_selection = add_selection;
    if (add_selection) drag.selection_base = selection.clips();
    else selection.clear();
    (void)p;
}

void EditController::begin_track_reorder(const Project& p, const TimelineGeometry& g, int track_id, int x, int y) {
    drag = {};
    drag.mode = DragMode::TrackReorder;
    drag.track_id = track_id;
    drag.origin_x = x;
    drag.origin_y = y;
    drag.current_x = x;
    drag.current_y = y;
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        if (p.tracks[i].id == track_id) {
            drag.source_track_index = static_cast<int>(i);
            drag.target_track_index = static_cast<int>(i);
            break;
        }
    }
    update_track_reorder(p, g, y);
}

void EditController::begin_bin_drag(int media_id, int x, int y) {
    drag = {};
    drag.mode = DragMode::BinMedia;
    drag.media_id = media_id;
    drag.origin_x = x;
    drag.origin_y = y;
    drag.current_x = x;
    drag.current_y = y;
}

double EditController::snap_time(const Project& p, double candidate, double threshold,
                                 const std::unordered_set<int>& ignore, double playhead) const {
    if (!p.snapping) return std::max(0.0, candidate);
    double best = candidate;
    double dist = threshold + 1.0;
    auto test = [&](double t) {
        double d = std::abs(candidate - t);
        if (d < dist && d <= threshold) {
            dist = d;
            best = t;
        }
    };
    test(0.0);
    test(playhead);
    for (const auto& marker : p.markers) test(marker.time);
    for (const auto& clip : p.clips) {
        if (!ignore.contains(clip.id)) {
            test(clip.start);
            test(clip.end());
            if (clip.snap_offset > 0.0 && clip.snap_offset < clip.duration) test(clip.start + clip.snap_offset);
        }
    }
    return std::max(0.0, best);
}

void EditController::update_marquee_selection(const Project& p, const TimelineGeometry& g) {
    const int left = std::min(drag.origin_x, drag.current_x);
    const int right = std::max(drag.origin_x, drag.current_x);
    const int top = std::min(drag.origin_y, drag.current_y);
    const int bottom = std::max(drag.origin_y, drag.current_y);

    std::unordered_set<int> hits;
    int track_y = g.y + g.ruler_h - g.scroll_y;
    for (const auto& track : p.tracks) {
        const int clip_top = track_y + 5;
        const int clip_bottom = track_y + track.height - 5;
        if (bottom >= clip_top && top <= clip_bottom) {
            for (const Clip* clip : p.clips_on_track(track.id)) {
                const int clip_left = x_at(g, clip->start);
                const int clip_right = x_at(g, clip->end());
                if (right >= clip_left && left <= clip_right) hits.insert(clip->id);
            }
        }
        track_y += track.height;
    }

    if (drag.additive_selection) hits.insert(drag.selection_base.begin(), drag.selection_base.end());
    selection.select_clips(p, hits, SelectionOperation::Replace, true);
}

void EditController::update_track_reorder(const Project& p, const TimelineGeometry& g, int y) {
    if (drag.source_track_index < 0 || drag.source_track_index >= static_cast<int>(p.tracks.size())) return;
    const TrackKind kind = p.tracks[static_cast<std::size_t>(drag.source_track_index)].kind;
    int track_y = g.y + g.ruler_h - g.scroll_y;
    int best_index = drag.source_track_index;
    int best_distance = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const auto& track = p.tracks[i];
        const int center = track_y + track.height / 2;
        if (track.kind == kind) {
            const int distance = std::abs(y - center);
            if (distance < best_distance) {
                best_distance = distance;
                best_index = static_cast<int>(i);
            }
        }
        track_y += track.height;
    }
    drag.target_track_index = best_index;
}

void EditController::update_drag(const Project& p, const TimelineGeometry& g, int x, int y, double playhead) {
    drag.current_x = x;
    drag.current_y = y;
    if (drag.mode == DragMode::Marquee) {
        update_marquee_selection(p, g);
        return;
    }
    if (drag.mode == DragMode::TrackReorder) {
        update_track_reorder(p, g, y);
        return;
    }
    if (drag.mode == DragMode::None || drag.mode == DragMode::BinMedia || drag.mode == DragMode::Playhead) return;

    const Clip* root = p.clip_by_id(drag.clip_id);
    if (!root || !drag.original.contains(root->id)) return;
    const auto ro = drag.original.at(root->id);
    double dt = (x - drag.origin_x) / g.px_per_sec;
    const double frame = 1.0 / std::max(1.0, p.settings.fps);
    const double threshold = 8.0 / g.px_per_sec;
    std::unordered_set<int> ignore;
    for (const auto& kv : drag.original) ignore.insert(kv.first);

    if (drag.mode == DragMode::FadeIn || drag.mode == DragMode::FadeOut) {
        auto v = ro;
        const double fps = std::max(1.0, p.settings.fps);
        if (drag.mode == DragMode::FadeIn) {
            const double raw = time_at(g, x) - ro.start;
            v.fade_in = std::clamp(std::round(raw * fps) / fps, 0.0, ro.duration);
        } else {
            const double raw = ro.start + ro.duration - time_at(g, x);
            v.fade_out = std::clamp(std::round(raw * fps) / fps, 0.0, ro.duration);
        }
        drag.preview[root->id] = v;
        return;
    }

    if (drag.mode == DragMode::Move) {
        const double root_snap = std::clamp(root->snap_offset, 0.0, ro.duration);
        const double candidate = ro.start + root_snap + dt;
        const double snapped = snap_time(p, candidate, threshold, ignore, playhead);
        dt = quantize_time(p, snapped - root_snap) - ro.start;
        double earliest = ro.start;
        double latest_end = ro.start + ro.duration;
        for (int id : drag.primary_ids) {
            earliest = std::min(earliest, drag.original.at(id).start);
            latest_end = std::max(latest_end, drag.original.at(id).start + drag.original.at(id).duration);
        }
        dt = std::clamp(dt, -earliest, kTimelineMaxSeconds - latest_end);
        for (int id : drag.primary_ids) {
            auto v = drag.original.at(id);
            v.start += dt;
            drag.preview[id] = v;
        }
        const int target = track_at(p, g, y);
        if (target && p.track_by_id(target) && p.track_by_id(root->track_id) && !p.track_by_id(target)->locked &&
            p.track_by_id(target)->kind == p.track_by_id(root->track_id)->kind) {
            const int source_position = kind_track_position(p, root->track_id);
            const int target_position = kind_track_position(p, target);
            const int track_delta = target_position - source_position;
            bool vertical_valid = source_position >= 0 && target_position >= 0;
            std::unordered_map<int, int> destinations;
            if (vertical_valid) {
                for (int id : drag.primary_ids) {
                    const Clip* clip = p.clip_by_id(id);
                    const Track* source_track = clip ? p.track_by_id(clip->track_id) : nullptr;
                    if (!clip || !source_track) { vertical_valid = false; break; }
                    const int position = kind_track_position(p, clip->track_id);
                    const Track* destination = kind_track_at(p, source_track->kind, position + track_delta);
                    if (!destination || destination->locked) { vertical_valid = false; break; }
                    destinations[id] = destination->id;
                }
            }
            if (vertical_valid) for (const auto& [id, track_id] : destinations) drag.preview[id].track_id = track_id;
        }
        return;
    }

    if (drag.mode == DragMode::TrimLeft) {
        double delta = quantize_time(p, snap_time(p, ro.start + dt, threshold, ignore, playhead)) - ro.start;
        double min_delta = -ro.start;
        double max_delta = ro.duration - frame;
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            min_delta = std::max(min_delta, -original.start);
            if (!c->loop) min_delta = std::max(min_delta, min_source_shift(*c, true));
            max_delta = std::min(max_delta, original.duration - frame);
        }
        delta = std::clamp(delta, min_delta, max_delta);
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            auto v = drag.original.at(id);
            v.start += delta;
            v.duration -= delta;
            v.in_point += delta * c->rate;
            drag.preview[id] = v;
        }
        return;
    }

    if (drag.mode == DragMode::TrimRight) {
        double delta = quantize_time(p, snap_time(p, ro.start + ro.duration + dt, threshold, ignore, playhead)) - (ro.start + ro.duration);
        double min_delta = frame - ro.duration;
        double max_delta = std::numeric_limits<double>::infinity();
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            min_delta = std::max(min_delta, frame - original.duration);
            const double extension = max_source_extension(p, *c, original.in_point, true);
            if (std::isfinite(extension)) max_delta = std::min(max_delta, extension);
        }
        max_delta = std::min(max_delta, kTimelineMaxSeconds - (ro.start + ro.duration));
        if (std::isfinite(max_delta)) delta = std::min(delta, max_delta);
        delta = std::max(delta, min_delta);
        for (int id : drag.primary_ids) {
            auto v = drag.original.at(id);
            v.duration += delta;
            drag.preview[id] = v;
        }
        return;
    }

    if (drag.mode == DragMode::Slip) {
        double min_delta = -std::numeric_limits<double>::infinity();
        double max_delta = std::numeric_limits<double>::infinity();
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            const double rate = std::max(0.0001, c->rate);
            min_delta = std::max(min_delta, c->loop ? -std::numeric_limits<double>::infinity() : min_source_shift(*c, p.slip_all_takes));
            if (!c->loop) {
                if (p.slip_all_takes) {
                    for (const auto& take : c->takes) {
                        const MediaItem* media = p.media_by_id(take.media_id);
                        if (!media || media->duration <= 0.0) continue;
                        const double max_in = std::max(0.0, media->duration - original.duration * rate);
                        max_delta = std::min(max_delta, (max_in - take.offset) / rate);
                    }
                } else if (const Take* take = c->active_take_ptr()) {
                    if (const MediaItem* media = p.media_by_id(take->media_id); media && media->duration > 0) {
                        const double max_in = std::max(0.0, media->duration - original.duration * rate);
                        max_delta = std::min(max_delta, (max_in - original.in_point) / rate);
                    }
                }
            }
        }
        double delta = dt;
        if (std::isfinite(min_delta)) delta = std::max(delta, min_delta);
        if (std::isfinite(max_delta)) delta = std::min(delta, max_delta);
        for (int id : drag.primary_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            auto v = drag.original.at(id);
            v.in_point += delta * c->rate;
            drag.preview[id] = v;
        }
        return;
    }

    if (drag.mode == DragMode::Roll) {
        double delta = quantize_time(p, snap_time(p, drag.anchor_time + dt, threshold, ignore, playhead)) - drag.anchor_time;
        double min_delta = -std::numeric_limits<double>::infinity();
        double max_delta = std::numeric_limits<double>::infinity();
        for (int id : drag.left_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            min_delta = std::max(min_delta, frame - original.duration);
            const double extension = max_source_extension(p, *c, original.in_point, true);
            if (std::isfinite(extension)) max_delta = std::min(max_delta, extension);
        }
        for (int id : drag.right_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            if (!c->loop) min_delta = std::max(min_delta, min_source_shift(*c, true));
            max_delta = std::min(max_delta, original.duration - frame);
        }
        delta = std::clamp(delta, min_delta, max_delta);
        for (int id : drag.left_neighbor_ids) {
            auto v = drag.original.at(id);
            v.duration += delta;
            drag.preview[id] = v;
        }
        for (int id : drag.right_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            auto v = drag.original.at(id);
            v.start += delta;
            v.duration -= delta;
            v.in_point += delta * c->rate;
            drag.preview[id] = v;
        }
        return;
    }

    if (drag.mode == DragMode::Slide) {
        dt = quantize_time(p, snap_time(p, ro.start + dt, threshold, ignore, playhead)) - ro.start;
        double min_delta = -ro.start;
        double max_delta = std::numeric_limits<double>::infinity();
        for (int id : drag.primary_ids) min_delta = std::max(min_delta, -drag.original.at(id).start);
        for (int id : drag.left_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            min_delta = std::max(min_delta, frame - original.duration);
            const double extension = max_source_extension(p, *c, original.in_point, true);
            if (std::isfinite(extension)) max_delta = std::min(max_delta, extension);
        }
        for (int id : drag.right_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            const auto& original = drag.original.at(id);
            if (!c) continue;
            if (!c->loop) min_delta = std::max(min_delta, min_source_shift(*c, true));
            max_delta = std::min(max_delta, original.duration - frame);
        }
        double selected_latest = 0.0;
        for (int id : drag.primary_ids) { const auto& original=drag.original.at(id); selected_latest=std::max(selected_latest, original.start+original.duration); }
        max_delta = std::min(max_delta, kTimelineMaxSeconds-selected_latest);
        if (std::isfinite(max_delta)) dt = std::min(dt, max_delta);
        dt = std::max(dt, min_delta);
        for (int id : drag.primary_ids) {
            auto v = drag.original.at(id);
            v.start += dt;
            drag.preview[id] = v;
        }
        for (int id : drag.left_neighbor_ids) {
            auto v = drag.original.at(id);
            v.duration += dt;
            drag.preview[id] = v;
        }
        for (int id : drag.right_neighbor_ids) {
            const Clip* c = p.clip_by_id(id);
            if (!c) continue;
            auto v = drag.original.at(id);
            v.start += dt;
            v.duration -= dt;
            v.in_point += dt * c->rate;
            drag.preview[id] = v;
        }
        return;
    }

    if (drag.mode == DragMode::RippleTrim) {
        if (drag.edge > 0) {
            double delta = quantize_time(p, snap_time(p, ro.start + ro.duration + dt, threshold, ignore, playhead)) - (ro.start + ro.duration);
            double min_delta = frame - ro.duration;
            double max_delta = std::numeric_limits<double>::infinity();
            for (int id : drag.primary_ids) {
                const Clip* c = p.clip_by_id(id);
                const auto& original = drag.original.at(id);
                if (!c) continue;
                min_delta = std::max(min_delta, frame - original.duration);
                const double extension = max_source_extension(p, *c, original.in_point, true);
                if (std::isfinite(extension)) max_delta = std::min(max_delta, extension);
            }
            double affected_latest=0.0;for(int id:drag.primary_ids){const auto&o=drag.original.at(id);affected_latest=std::max(affected_latest,o.start+o.duration);}for(int id:drag.ripple_ids){const auto&o=drag.original.at(id);affected_latest=std::max(affected_latest,o.start+o.duration);}
            max_delta=std::min(max_delta,kTimelineMaxSeconds-affected_latest);
            if (std::isfinite(max_delta)) delta = std::min(delta, max_delta);
            delta = std::max(delta, min_delta);
            for (int id : drag.primary_ids) {
                auto v = drag.original.at(id);
                v.duration += delta;
                drag.preview[id] = v;
            }
            for (int id : drag.ripple_ids) {
                auto v = drag.original.at(id);
                v.start += delta;
                drag.preview[id] = v;
            }
        } else {
            double delta = quantize_time(p, snap_time(p, ro.start + dt, threshold, ignore, playhead)) - ro.start;
            double min_delta = -std::numeric_limits<double>::infinity();
            double max_delta = ro.duration - frame;
            for (int id : drag.primary_ids) {
                const Clip* c = p.clip_by_id(id);
                const auto& original = drag.original.at(id);
                if (!c) continue;
                if (!c->loop) min_delta = std::max(min_delta, min_source_shift(*c, true));
                max_delta = std::min(max_delta, original.duration - frame);
            }
            double ripple_latest=0.0;for(int id:drag.ripple_ids){const auto&o=drag.original.at(id);ripple_latest=std::max(ripple_latest,o.start+o.duration);}
            min_delta=std::max(min_delta,ripple_latest-kTimelineMaxSeconds);
            delta = std::clamp(delta, min_delta, max_delta);
            for (int id : drag.primary_ids) {
                const Clip* c = p.clip_by_id(id);
                if (!c) continue;
                auto v = drag.original.at(id);
                v.duration -= delta;
                v.in_point += delta * c->rate;
                drag.preview[id] = v;
            }
            for (int id : drag.ripple_ids) {
                auto v = drag.original.at(id);
                v.start -= delta;
                drag.preview[id] = v;
            }
        }
    }
}

bool EditController::commit_drag(Project& p, UndoStack& undo, const TimelineGeometry& g, int x, int y, double playhead) {
    update_drag(p, g, x, y, playhead);
    if (drag.mode == DragMode::Marquee) {
        cancel_drag();
        return false;
    }
    if (drag.mode == DragMode::TrackReorder) {
        if (drag.source_track_index < 0 || drag.target_track_index < 0 ||
            drag.source_track_index == drag.target_track_index) {
            cancel_drag();
            return false;
        }
        undo.begin(p);
        Track moving = p.tracks.at(static_cast<std::size_t>(drag.source_track_index));
        p.tracks.erase(p.tracks.begin() + drag.source_track_index);
        const int target = std::clamp(drag.target_track_index, 0, static_cast<int>(p.tracks.size()));
        p.tracks.insert(p.tracks.begin() + target, std::move(moving));
        const bool changed = undo.commit(p);
        cancel_drag();
        return changed;
    }
    if (drag.mode == DragMode::BinMedia) {
        if (x < g.x + g.header_w || x >= g.x + g.width || y < g.y + g.ruler_h || y >= g.y + g.height) {
            cancel_drag(); return false;
        }
        const int tid = track_at(p, g, y);
        undo.begin(p);
        const double start = quantize_time(p, snap_time(p, time_at(g, x), 8.0 / g.px_per_sec, {}, playhead));
        const int added = p.place_media(drag.media_id, start, tid, tid == 0);
        if (!added) { undo.cancel(); cancel_drag(); return false; }
        selection.select_clip(p, added, SelectionOperation::Replace, true);
        const bool changed = undo.commit(p);
        cancel_drag();
        return changed;
    }
    if (drag.mode == DragMode::None || drag.mode == DragMode::Playhead) return false;

    undo.begin(p);
    for (const auto& kv : drag.preview) {
        if (auto* c = p.clip_by_id(kv.first)) {
            const double source_delta = kv.second.in_point - c->source_offset();
            if (std::abs(source_delta) > 1e-12) {
                shift_take_offsets(*c, source_delta, drag.mode == DragMode::Slip ? p.slip_all_takes : true);
            }
            c->start = kv.second.start;
            c->duration = kv.second.duration;
            c->fade_in = std::clamp(kv.second.fade_in, 0.0, c->duration);
            c->fade_out = std::clamp(kv.second.fade_out, 0.0, c->duration);
            c->track_id = kv.second.track_id;
        }
    }
    const bool changed = undo.commit(p);
    cancel_drag();
    return changed;
}

void EditController::cancel_drag() { drag = {}; }

bool EditController::split_selected(Project& p, UndoStack& undo, double playhead) {
    playhead = quantize_time(p, playhead);
    std::vector<int> ids(selection.clips().begin(), selection.clips().end());
    if (ids.empty()) {
        // VEGAS-style useful default: with no explicit event selection, Split
        // acts on events intersecting the edit cursor.
        for (const auto& clip : p.clips) if (clip.contains(playhead)) ids.push_back(clip.id);
    } else if (!p.ignore_event_grouping) {
        std::unordered_set<int> expanded(ids.begin(), ids.end());
        for (int id : ids) {
            const Clip* clip = p.clip_by_id(id);
            if (!clip || clip->group_id <= 0) continue;
            for (const Clip* grouped : p.clips_in_group(clip->group_id)) expanded.insert(grouped->id);
        }
        ids.assign(expanded.begin(), expanded.end());
    }
    if (ids.empty()) return false;
    std::unordered_set<int> selected(ids.begin(), ids.end());
    if (!all_editable(p, selected)) return false;

    undo.begin(p);
    bool any = false;
    std::unordered_set<int> rights;
    int primary_right = 0;
    std::unordered_map<int, int> right_groups;
    for (int id : ids) {
        const Clip* before = p.clip_by_id(id);
        const int old_group = before ? before->group_id : 0;
        int right = 0;
        if (p.split_clip(id, playhead, &right)) {
            any = true;
            if (!p.ignore_event_grouping && old_group > 0) {
                auto [it, inserted] = right_groups.emplace(old_group, 0);
                if (inserted) it->second = p.next_group_id();
                if (Clip* right_clip = p.clip_by_id(right)) right_clip->group_id = it->second;
            }
            rights.insert(right);
            if (id == selection.primary_clip()) primary_right = right;
        }
    }
    if (!any) {
        undo.cancel();
        return false;
    }
    undo.commit(p);
    selection.select_clips(p, rights, SelectionOperation::Replace, false, primary_right);
    return true;
}

bool EditController::delete_selected(Project& p, UndoStack& undo) {
    std::unordered_set<int> ids = selection.clips();
    if (ids.empty() || !all_editable(p, ids)) return false;

    undo.begin(p);
    double earliest = std::numeric_limits<double>::max();
    double latest = 0;
    std::optional<int> only_track;
    for (int id : ids) {
        if (auto* c = p.clip_by_id(id)) {
            earliest = std::min(earliest, c->start);
            latest = std::max(latest, c->end());
            if (!only_track) only_track = c->track_id;
            else if (*only_track != c->track_id) only_track.reset();
        }
    }
    for (int id : ids) p.erase_clip(id);
    if (p.ripple != RippleMode::Off && earliest < std::numeric_limits<double>::max()) {
        p.ripple_close(latest, latest - earliest, p.ripple == RippleMode::AffectedTrack ? only_track : std::nullopt);
    }
    const bool changed = undo.commit(p);
    selection.clear();
    return changed;
}

bool EditController::nudge_selected(Project& p, UndoStack& undo, double delta) {
    const std::unordered_set<int> ids = selection.clips();
    if (ids.empty() || !all_editable(p, ids)) return false;
    double min_delta=-std::numeric_limits<double>::infinity(),max_delta=std::numeric_limits<double>::infinity();
    for(int id:ids)if(const auto*c=p.clip_by_id(id)){min_delta=std::max(min_delta,-c->start);max_delta=std::min(max_delta,kTimelineMaxSeconds-c->end());}
    delta=std::clamp(delta,min_delta,max_delta);
    undo.begin(p);
    for (int id : ids) {
        if (auto* c = p.clip_by_id(id)) c->start += delta;
    }
    return undo.commit(p);
}

bool EditController::copy_selected(const Project& p, ClipClipboard& clipboard) const {
    clipboard.clear();
    if (selection.empty()) return false;
    double origin = std::numeric_limits<double>::max();
    for (int id : selection.clips()) {
        if (const Clip* c = p.clip_by_id(id)) {
            clipboard.clips.push_back(*c);
            origin = std::min(origin, c->start);
        }
    }
    if (clipboard.clips.empty()) return false;
    std::sort(clipboard.clips.begin(), clipboard.clips.end(), [](const Clip& a, const Clip& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.track_id != b.track_id) return a.track_id < b.track_id;
        return a.id < b.id;
    });
    clipboard.origin = origin;
    return true;
}

bool EditController::cut_selected(Project& p, UndoStack& undo, ClipClipboard& clipboard) {
    if (!copy_selected(p, clipboard)) return false;
    return delete_selected(p, undo);
}

bool EditController::paste(Project& p, UndoStack& undo, const ClipClipboard& clipboard, double at_time) {
    if (clipboard.empty()) return false;
    for (const Clip& source : clipboard.clips) {
        const Track* track = p.track_by_id(source.track_id);
        if (!track || track->locked) return false;
    }
    undo.begin(p);
    std::unordered_map<int, int> group_map;
    std::unordered_set<int> pasted;
    int primary = 0;
    for (const Clip& source : clipboard.clips) {
        int group = 0;
        if (source.group_id > 0) {
            auto [it, inserted] = group_map.emplace(source.group_id, 0);
            if (inserted) it->second = p.next_group_id();
            group = it->second;
        }
        Clip& target = p.add_clip(source.media_id(), source.track_id,
                                  std::max(0.0, at_time + (source.start - clipboard.origin)),
                                  source.source_offset(), source.duration, group);
        copy_clip_properties(source, target);
        pasted.insert(target.id);
        if (!primary) primary = target.id;
    }
    const bool changed = undo.commit(p);
    if (changed) selection.select_clips(p, pasted, SelectionOperation::Replace, false, primary);
    return changed;
}

bool EditController::toggle_lock_selected(Project& p, UndoStack& undo) {
    const auto ids = selection.clips();
    if (ids.empty()) return false;
    bool all_locked = true;
    for (int id : ids) if (const Clip* c = p.clip_by_id(id)) all_locked = all_locked && c->locked;
    undo.begin(p);
    for (int id : ids) if (Clip* c = p.clip_by_id(id)) c->locked = !all_locked;
    return undo.commit(p);
}

bool EditController::toggle_loop_selected(Project& p, UndoStack& undo) {
    const auto ids = selection.clips();
    if (ids.empty() || !all_editable(p, ids)) return false;
    bool all_loop = true;
    for (int id : ids) if (const Clip* clip = p.clip_by_id(id)) all_loop = all_loop && clip->loop;
    undo.begin(p);
    for (int id : ids) if (Clip* clip = p.clip_by_id(id)) clip->loop = !all_loop;
    return undo.commit(p);
}

bool EditController::cycle_active_take(Project& p, UndoStack& undo, int direction) {
    const auto ids = selection.clips();
    if (ids.empty() || direction == 0 || !all_editable(p, ids)) return false;
    undo.begin(p);
    bool touched = false;
    for (int id : ids) {
        Clip* clip = p.clip_by_id(id);
        if (!clip || clip->takes.size() < 2) continue;
        const int count = static_cast<int>(clip->takes.size());
        clip->active_take = (clip->active_take + (direction > 0 ? 1 : -1) + count) % count;
        touched = true;
    }
    if (!touched) { undo.cancel(); return false; }
    return undo.commit(p);
}

bool EditController::trim_start_to_cursor(Project& p, UndoStack& undo, double playhead) {
    const auto ids = selection.clips();
    if (ids.empty() || !all_editable(p, ids)) return false;
    playhead = quantize_time(p, playhead);
    const double frame = 1.0 / std::max(1.0, p.settings.fps);
    undo.begin(p);
    bool touched = false;
    for (int id : ids) {
        Clip* clip = p.clip_by_id(id);
        if (!clip || playhead <= clip->start + 1e-9 || playhead >= clip->end() - frame + 1e-9) continue;
        const double delta = playhead - clip->start;
        clip->start = playhead;
        clip->duration -= delta;
        shift_take_offsets(*clip, delta * clip->rate, true);
        clip->snap_offset = std::max(0.0, clip->snap_offset - delta);
        touched = true;
    }
    if (!touched) { undo.cancel(); return false; }
    return undo.commit(p);
}

bool EditController::trim_end_to_cursor(Project& p, UndoStack& undo, double playhead) {
    const auto ids = selection.clips();
    if (ids.empty() || !all_editable(p, ids)) return false;
    playhead = quantize_time(p, playhead);
    const double frame = 1.0 / std::max(1.0, p.settings.fps);
    undo.begin(p);
    bool touched = false;
    for (int id : ids) {
        Clip* clip = p.clip_by_id(id);
        if (!clip || playhead <= clip->start + frame - 1e-9 || playhead >= clip->end() - 1e-9) continue;
        clip->duration = playhead - clip->start;
        clip->snap_offset = std::min(clip->snap_offset, clip->duration);
        touched = true;
    }
    if (!touched) { undo.cancel(); return false; }
    return undo.commit(p);
}

bool EditController::insert_media(Project& p, UndoStack& undo, int media_id, double source_in, double source_out, double at_time) {
    const MediaItem* media = p.media_by_id(media_id);
    if (!media) return false;
    at_time = quantize_time(p, at_time);
    const double end = source_out > source_in ? source_out : media->duration;
    const double duration = end - source_in;
    if (duration <= 0.0) return false;
    const auto tracks = destination_tracks(p, *media);
    if (tracks.empty() || at_time + duration > kTimelineMaxSeconds + 1e-9) return false;
    for(int track_id:tracks)for(const Clip* clip:p.clips_on_track(track_id))if(clip->start>=at_time-1e-9&&clip->end()+duration>kTimelineMaxSeconds+1e-9)return false;

    // Preflight: an insert ripples everything at/after the insertion point, so a locked event blocks it.
    for (int track_id : tracks) {
        for (const Clip* clip : p.clips_on_track(track_id)) {
            if ((clip->start >= at_time - 1e-9 || (clip->start < at_time && clip->end() > at_time)) && clip->locked) return false;
        }
    }

    undo.begin(p);
    std::unordered_map<int, int> right_group_map;
    for (int track_id : tracks) {
        std::vector<int> ids;
        for (const Clip* clip : p.clips_on_track(track_id)) ids.push_back(clip->id);
        for (int id : ids) {
            Clip* clip = p.clip_by_id(id);
            if (!clip || !(clip->start < at_time && clip->end() > at_time)) continue;
            const int old_group = clip->group_id;
            int right_id = 0;
            if (p.split_clip(id, at_time, &right_id) && old_group > 0) {
                auto [it, inserted] = right_group_map.emplace(old_group, 0);
                if (inserted) it->second = p.next_group_id();
                if (Clip* right = p.clip_by_id(right_id)) right->group_id = it->second;
            }
        }
    }
    for (int track_id : tracks) {
        for (Clip* clip : p.clips_on_track(track_id)) if (clip->start >= at_time - 1e-9) clip->start += duration;
    }
    const int group = media->has_video && media->has_audio && tracks.size() > 1 ? p.next_group_id() : 0;
    std::unordered_set<int> created;
    int primary = 0;
    for (int track_id : tracks) {
        Clip& clip = p.add_clip(media_id, track_id, at_time, source_in, duration, group);
        created.insert(clip.id);
        if (!primary) primary = clip.id;
    }
    const bool changed = undo.commit(p);
    if (changed) selection.select_clips(p, created, SelectionOperation::Replace, false, primary);
    return changed;
}

bool EditController::overwrite_media(Project& p, UndoStack& undo, int media_id, double source_in, double source_out, double at_time) {
    const MediaItem* media = p.media_by_id(media_id);
    if (!media) return false;
    at_time = quantize_time(p, at_time);
    const double source_end = source_out > source_in ? source_out : media->duration;
    const double duration = source_end - source_in;
    if (duration <= 0.0) return false;
    const double overwrite_end = at_time + duration;
    if(overwrite_end > kTimelineMaxSeconds + 1e-9) return false;
    const auto tracks = destination_tracks(p, *media);
    if (tracks.empty()) return false;

    for (int track_id : tracks) {
        for (const Clip* clip : p.clips_on_track(track_id)) {
            if (clip->end() > at_time && clip->start < overwrite_end && clip->locked) return false;
        }
    }

    undo.begin(p);
    std::unordered_map<int, int> right_group_map;
    for (int track_id : tracks) {
        std::vector<int> ids;
        for (const Clip* clip : p.clips_on_track(track_id)) ids.push_back(clip->id);
        for (int id : ids) {
            Clip* clip = p.clip_by_id(id);
            if (!clip || clip->end() <= at_time || clip->start >= overwrite_end) continue;
            const Clip original = *clip;
            if (original.start < at_time && original.end() > overwrite_end) {
                clip->duration = at_time - original.start;
                int right_group = original.group_id;
                if (original.group_id > 0) {
                    auto [it, inserted] = right_group_map.emplace(original.group_id, 0);
                    if (inserted) it->second = p.next_group_id();
                    right_group = it->second;
                }
                Clip& right = p.add_clip(original.media_id(), original.track_id, overwrite_end,
                                         original.source_offset() + (overwrite_end - original.start) * original.rate,
                                         original.end() - overwrite_end, right_group);
                copy_clip_properties(original, right);
                shift_take_offsets(right, (overwrite_end - original.start) * original.rate, true);
            } else if (original.start < at_time && original.end() > at_time) {
                clip->duration = at_time - original.start;
            } else if (original.start < overwrite_end && original.end() > overwrite_end) {
                clip->start = overwrite_end;
                clip->duration = original.end() - overwrite_end;
                shift_take_offsets(*clip, (overwrite_end - original.start) * original.rate, true);
            } else {
                p.erase_clip(id);
            }
        }
    }

    const int group = media->has_video && media->has_audio && tracks.size() > 1 ? p.next_group_id() : 0;
    std::unordered_set<int> created;
    int primary = 0;
    for (int track_id : tracks) {
        Clip& clip = p.add_clip(media_id, track_id, at_time, source_in, duration, group);
        created.insert(clip.id);
        if (!primary) primary = clip.id;
    }
    const bool changed = undo.commit(p);
    if (changed) selection.select_clips(p, created, SelectionOperation::Replace, false, primary);
    return changed;
}

} // namespace koralo
