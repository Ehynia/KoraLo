#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace koralo {
struct Frame;
struct BrushStroke {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0, radius = .06, hardness = .75;
  bool erase = true;
};
std::string encode_brush_stroke(const BrushStroke &);
class PaintMaskRaster {
public:
  void update(std::string_view strokes, int width, int height);
  const std::vector<float> &coverage() const { return mask_; }
  std::uint64_t segments_rasterized() const { return rasterized_; }
  std::uint64_t rebuilds() const { return rebuilds_; }

private:
  std::string text_;
  std::vector<float> mask_;
  int width_ = 0, height_ = 0, count_ = 0;
  std::uint64_t rasterized_ = 0, rebuilds_ = 0;
};
void apply_paint_mask(Frame &, std::string_view strokes, int cache_id = 0,
                      double amount = 1);
} // namespace koralo
