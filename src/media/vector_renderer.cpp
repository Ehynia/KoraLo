#include "vector_renderer.hpp"
#include "media_engine.hpp"
#include "paint_mask.hpp"
#include <algorithm>
#include <cmath>
namespace koralo {
namespace {
void blend(Frame &f, std::size_t p, const VectorColor &c, double coverage) {
  const double a = std::clamp(c.a * coverage, 0.0, 1.0),
               ba = f.alpha[p] / 255.0, oa = a + ba * (1 - a);
  if (oa <= 0)
    return;
  const double color[3] = {c.r, c.g, c.b};
  for (int ch = 0; ch < 3; ++ch)
    f.rgb[p * 3 + ch] = std::uint8_t(std::lround(std::clamp(
        (color[ch] * 255 * a + f.rgb[p * 3 + ch] * ba * (1 - a)) / oa, 0.0,
        255.0)));
  f.alpha[p] = std::uint8_t(std::lround(oa * 255));
}
void draw_shape(Frame &f, const VectorShape &shape, double time) {
  if (!shape.visible || shape.nodes.size() < 2)
    return;
  const int w = f.width, h = f.height;
  auto path = flatten_vector_path(shape, time, 20, double(w) / h);
  for (auto &p : path) {
    p.x *= w;
    p.y *= h;
  }
  const double opacity = std::clamp(shape.opacity.value_at(time), 0.0, 1.0);
  if (opacity < 1e-6)
    return;
  const auto n = std::size_t(w) * h;
  std::vector<std::uint8_t> coverage(n, 0);
  double ymin = h, ymax = 0;
  for (const auto &p : path) {
    ymin = std::min(ymin, p.y);
    ymax = std::max(ymax, p.y);
  }
  const int top = std::clamp(int(std::floor(ymin)), 0, h),
            bottom = std::clamp(int(std::ceil(ymax)), 0, h);
  if (shape.closed && shape.fill_enabled) {
    std::vector<double> crossings;
    crossings.reserve(path.size());
    for (int y = top; y < bottom; ++y)
      for (int sample = 0; sample < 2; ++sample) {
        const double scan = y + (sample + .5) / 2;
        crossings.clear();
        for (std::size_t i = 1; i < path.size(); ++i) {
          const auto a = path[i - 1], b = path[i];
          if ((a.y <= scan && b.y > scan) || (b.y <= scan && a.y > scan))
            crossings.push_back(a.x + (scan - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(crossings.begin(), crossings.end());
        for (std::size_t i = 1; i < crossings.size(); i += 2) {
          const double l = crossings[i - 1], r = crossings[i];
          for (int x = std::clamp(int(std::floor(l)), 0, w);
               x < std::clamp(int(std::ceil(r)), 0, w); ++x) {
            const double covered = std::clamp(
                std::min(r, double(x + 1)) - std::max(l, double(x)), 0.0, 1.0);
            auto &v = coverage[std::size_t(y) * w + x];
            v = std::uint8_t(
                std::min(255, int(v) + int(std::lround(127.5 * covered))));
          }
        }
      }
    VectorColor color = shape.fill;
    color.a *= opacity;
    for (std::size_t p = 0; p < n; ++p)
      if (coverage[p])
        blend(f, p, color, coverage[p] / 255.0);
  }
  const double rad = shape.stroke_width * std::min(w, h) *
                     std::clamp(shape.scale.value_at(time), .01, 20.0) * .5;
  if (rad < .05 || shape.stroke.a <= 0)
    return;
  std::fill(coverage.begin(), coverage.end(), 0);
  for (std::size_t i = 1; i < path.size(); ++i) {
    const auto a = path[i - 1], b = path[i];
    const double vx = b.x - a.x, vy = b.y - a.y, len = vx * vx + vy * vy;
    const int l = std::clamp(int(std::floor(std::min(a.x, b.x) - rad - 1)), 0,
                             w),
              r = std::clamp(int(std::ceil(std::max(a.x, b.x) + rad + 1)), 0,
                             w),
              t = std::clamp(int(std::floor(std::min(a.y, b.y) - rad - 1)), 0,
                             h),
              bt = std::clamp(int(std::ceil(std::max(a.y, b.y) + rad + 1)), 0,
                              h);
    for (int y = t; y < bt; ++y)
      for (int x = l; x < r; ++x) {
        const double u = len > 1e-12 ? std::clamp(((x + .5 - a.x) * vx +
                                                   (y + .5 - a.y) * vy) /
                                                      len,
                                                  0.0, 1.0)
                                     : 0,
                     dx = x + .5 - a.x - u * vx, dy = y + .5 - a.y - u * vy;
        const auto cv = std::uint8_t(std::lround(
            std::clamp(rad + .5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0) *
            255));
        auto &p = coverage[std::size_t(y) * w + x];
        p = std::max(p, cv);
      }
  }
  VectorColor color = shape.stroke;
  color.a *= opacity;
  for (std::size_t p = 0; p < n; ++p)
    if (coverage[p])
      blend(f, p, color, coverage[p] / 255.0);
}
} // namespace
Frame render_vector_scene(const VectorScene &s, int w, int h, double time) {
  Frame f;
  if (w < 1 || h < 1 || w > 8192 || h > 8192 ||
      std::size_t(w) * h > 16 * 1024 * 1024 || !std::isfinite(time))
    return f;
  f.width = w;
  f.height = h;
  f.rgb.assign(std::size_t(w) * h * 3, 0);
  f.alpha.assign(std::size_t(w) * h, 0);
  if (!s.enabled)
    return f;
  for (const auto &shape : s.shapes)
    draw_shape(f, shape, time);
  if (!s.mask_strokes.empty())
    apply_paint_mask(f, s.mask_strokes, -42, s.mask_amount.value_at(time));
  if (s.crop_enabled) {
    const double left = std::clamp(s.crop_x.value_at(time), 0.0, 1.0) * w,
                 top = std::clamp(s.crop_y.value_at(time), 0.0, 1.0) * h,
                 right =
                     left + std::clamp(s.crop_w.value_at(time), .001, 1.0) * w,
                 bottom =
                     top + std::clamp(s.crop_h.value_at(time), .001, 1.0) * h;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (x + .5 < left || x + .5 >= right || y + .5 < top ||
            y + .5 >= bottom)
          f.alpha[std::size_t(y) * w + x] = 0;
  }
  return f;
}
} // namespace koralo
