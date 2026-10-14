#include "paint_mask.hpp"
#include "media_engine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
namespace koralo {
std::string encode_brush_stroke(const BrushStroke &s) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(6) << s.x0 << ' ' << s.y0 << ' '
      << s.x1 << ' ' << s.y1 << ' ' << s.radius << ' ' << s.hardness << ' '
      << (s.erase ? 1 : 0) << '\n';
  return out.str();
}
void PaintMaskRaster::update(std::string_view text, int w, int h) {
  if (w < 1 || h < 1 || std::size_t(w) * h > 16 * 1024 * 1024)
    return;
  const bool append = w == width_ && h == height_ &&
                      text.size() >= text_.size() &&
                      text.substr(0, text_.size()) == text_;
  const std::size_t offset = append ? text_.size() : 0;
  if (!append) {
    width_ = w;
    height_ = h;
    mask_.assign(std::size_t(w) * h, 1.f);
    count_ = 0;
    ++rebuilds_;
  }
  if (text.size() == offset)
    return;
  std::istringstream in{std::string(text.substr(offset))};
  in.imbue(std::locale::classic());
  float x0, y0, x1, y1, r, hard, erase;
  while (count_ < 4096 && in >> x0 >> y0 >> x1 >> y1 >> r >> hard >> erase) {
    ++count_;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
        !std::isfinite(y1) || !std::isfinite(r) || !std::isfinite(hard) ||
        !std::isfinite(erase) || r <= 0)
      continue;
    const float ax = std::clamp(x0, 0.f, 1.f) * w,
                ay = std::clamp(y0, 0.f, 1.f) * h,
                bx = std::clamp(x1, 0.f, 1.f) * w,
                by = std::clamp(y1, 0.f, 1.f) * h;
    const float rad = std::clamp(r, .001f, .5f) * std::min(w, h), vx = bx - ax,
                vy = by - ay, len = vx * vx + vy * vy;
    hard = std::clamp(hard, 0.f, 1.f);
    const int l = std::clamp(int(std::min(ax, bx) - rad - 1), 0, w),
              right = std::clamp(int(std::max(ax, bx) + rad + 2), 0, w),
              top = std::clamp(int(std::min(ay, by) - rad - 1), 0, h),
              bottom = std::clamp(int(std::max(ay, by) + rad + 2), 0, h);
    for (int y = top; y < bottom; ++y)
      for (int x = l; x < right; ++x) {
        float u = len > 0
                      ? std::clamp(((x + .5f - ax) * vx + (y + .5f - ay) * vy) /
                                       len,
                                   0.f, 1.f)
                      : 0,
              dx = x + .5f - ax - u * vx, dy = y + .5f - ay - u * vy,
              d2 = dx * dx + dy * dy;
        if (d2 > rad * rad)
          continue;
        float a =
            hard >= .999f
                ? 1.f
                : std::clamp((1 - std::sqrt(d2) / rad) / (1 - hard), 0.f, 1.f);
        auto &value = mask_[std::size_t(y) * w + x];
        value = erase > .5f ? value * (1 - a) : value + (1 - value) * a;
      }
    ++rasterized_;
  }
  text_.assign(text);
}
void apply_paint_mask(Frame &f, std::string_view text, int id, double amount) {
  const auto n = std::size_t(f.width) * f.height;
  if (f.rgb.size() != n * 3 || !n)
    return;
  struct Entry {
    int id = -1, w = 0, h = 0;
    std::uint64_t stamp = 0;
    PaintMaskRaster raster;
  };
  // Per-worker bounded LRU: no locks, no reparsing/rerasterizing history.
  thread_local std::array<Entry, 3> entries;
  thread_local std::uint64_t clock = 0;
  auto *slot = &entries.front();
  for (auto &e : entries) {
    if (e.id == id && e.w == f.width && e.h == f.height) {
      slot = &e;
      break;
    }
    if (e.stamp < slot->stamp)
      slot = &e;
  }
  if (slot->id != id || slot->w != f.width || slot->h != f.height) {
    slot->raster = {};
    slot->id = id;
    slot->w = f.width;
    slot->h = f.height;
  }
  slot->stamp = ++clock;
  slot->raster.update(text, f.width, f.height);
  const auto &mask = slot->raster.coverage();
  if (mask.size() != n)
    return;
  if (f.alpha.size() != n)
    f.alpha.assign(n, 255);
  const float strength = float(std::clamp(amount, 0.0, 1.0));
  for (std::size_t i = 0; i < n; ++i)
    f.alpha[i] = std::uint8_t(
        std::lround(f.alpha[i] * (1 - strength + strength * mask[i])));
}
} // namespace koralo
