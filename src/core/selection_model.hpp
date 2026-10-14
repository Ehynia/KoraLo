#pragma once

#include "project.hpp"
#include <unordered_set>
#include <vector>

namespace koralo {

enum class SelectionOperation { Replace, Add };

class SelectionModel {
public:
    [[nodiscard]] bool empty() const { return clips_.empty(); }
    [[nodiscard]] bool contains(int clip_id) const { return clips_.contains(clip_id); }
    [[nodiscard]] int primary_clip() const { return primary_clip_; }
    [[nodiscard]] const std::unordered_set<int>& clips() const { return clips_; }

    void clear();
    void select_clip(const Project& project, int clip_id, SelectionOperation operation = SelectionOperation::Replace,
                     bool group_aware = true);
    void select_clips(const Project& project, const std::unordered_set<int>& clip_ids,
                      SelectionOperation operation = SelectionOperation::Replace, bool group_aware = true,
                      int primary_hint = 0);
    void retain_existing(const Project& project);

private:
    static std::unordered_set<int> expand_groups(const Project& project, const std::unordered_set<int>& clip_ids);

    int primary_clip_ = 0;
    std::unordered_set<int> clips_;
};

} // namespace koralo
