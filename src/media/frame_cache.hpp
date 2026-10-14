#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace koralo {

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
    // Optional straight alpha plane used by keying/compositing preview. Most
    // decoded frames stay RGB-only; effects allocate alpha only when needed.
    std::vector<std::uint8_t> alpha;
};

class FrameCache {
public:
    explicit FrameCache(std::size_t max_items = 96,
                        std::size_t max_bytes = 128 * 1024 * 1024)
        : max_items_(max_items), max_bytes_(max_bytes) {}

    std::optional<Frame> get(const std::string& key);
    void put(std::string key, Frame frame);
    void clear();

private:
    struct Entry {
        Frame frame;
        std::uint64_t stamp = 0;
    };

    std::size_t max_items_;
    std::size_t max_bytes_;
    std::size_t used_bytes_ = 0;
    std::uint64_t stamp_ = 0;
    std::unordered_map<std::string, Entry> map_;
};

} // namespace koralo
