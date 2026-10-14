#include "selection_model.hpp"

namespace koralo {

void SelectionModel::clear() {
    primary_clip_ = 0;
    clips_.clear();
}

std::unordered_set<int> SelectionModel::expand_groups(const Project& project, const std::unordered_set<int>& clip_ids) {
    std::unordered_set<int> expanded;
    for (int id : clip_ids) {
        const Clip* clip = project.clip_by_id(id);
        if (!clip) continue;
        expanded.insert(id);
        if (project.ignore_event_grouping || clip->group_id <= 0) continue;
        for (const Clip* grouped : project.clips_in_group(clip->group_id)) expanded.insert(grouped->id);
    }
    return expanded;
}

void SelectionModel::select_clip(const Project& project, int clip_id, SelectionOperation operation, bool group_aware) {
    std::unordered_set<int> ids;
    if (clip_id != 0) ids.insert(clip_id);
    select_clips(project, ids, operation, group_aware, clip_id);
}

void SelectionModel::select_clips(const Project& project, const std::unordered_set<int>& clip_ids,
                                  SelectionOperation operation, bool group_aware, int primary_hint) {
    std::unordered_set<int> incoming = group_aware ? expand_groups(project, clip_ids) : clip_ids;
    if (operation == SelectionOperation::Replace) clips_.clear();
    clips_.insert(incoming.begin(), incoming.end());

    if (primary_hint != 0 && clips_.contains(primary_hint)) primary_clip_ = primary_hint;
    else if (primary_clip_ == 0 || !clips_.contains(primary_clip_)) primary_clip_ = clips_.empty() ? 0 : *clips_.begin();
}

void SelectionModel::retain_existing(const Project& project) {
    for (auto it = clips_.begin(); it != clips_.end();) {
        if (!project.clip_by_id(*it)) it = clips_.erase(it);
        else ++it;
    }
    if (primary_clip_ != 0 && !clips_.contains(primary_clip_)) primary_clip_ = clips_.empty() ? 0 : *clips_.begin();
}

} // namespace koralo
