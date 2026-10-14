// Spatial CPU port of the user-supplied Anime Line Restore V45, 1.py.
// No Python bootstrap, process termination or dependency installation is run.
#include "native_effects.hpp"
#include "native_cuda.hpp"
#include "export_parallelism.hpp"
#include "media_engine.hpp"
#include "paint_mask.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <latch>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
namespace koralo {
namespace {
using Plane = std::vector<float>;
using RGB = std::array<Plane, 3>;
class WorkerThreadPool {
public:
    explicit WorkerThreadPool(unsigned threads) : stop_(false) {
        unsigned count = std::max(1u, threads);
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~WorkerThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    static WorkerThreadPool& instance() {
        static unsigned hc = std::max(1u, std::thread::hardware_concurrency());
        static WorkerThreadPool pool(std::clamp(hc, 2u, 16u));
        return pool;
    }

private:
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stop_;
};

template<class F> void parallel_rows(int width,int height,F&& fn){
  const std::uint64_t pixels=std::uint64_t(std::max(0,width))*std::max(0,height);
  const unsigned hc=export_inner_thread_budget ? export_inner_thread_budget : std::thread::hardware_concurrency();
  const int workers=std::min(height,int(std::clamp(hc?hc:4u,export_inner_thread_budget?1u:2u,16u)));
  if(pixels<256u*256u||height<32||workers<2){fn(0,height);return;}
  
  std::latch done(workers - 1);
  auto& pool = WorkerThreadPool::instance();
  for (int i = 1; i < workers; ++i) {
    const int y0 = height * i / workers, y1 = height * (i + 1) / workers;
    pool.enqueue([&fn, y0, y1, &done]() {
      fn(y0, y1);
      done.count_down();
    });
  }

  // Calling thread executes chunk 0 directly
  fn(0, height / workers);

  done.wait();
}
float clamp(float x) { return std::clamp(x, 0.f, 1.f); }
int reflect(int x, int n) {
  if (n < 2)
    return 0;
  while (x < 0 || x >= n)
    x = x < 0 ? -x : 2 * n - 2 - x;
  return x;
}
Plane gaussian(const Plane &p, int w, int h, float sigma) {
  if (sigma <= 0)
    return p;
  // Large separable passes can run on CUDA during GPU-enabled native exports.
  // Small images and any CUDA failure use the identical CPU algorithm.
  if(native_cuda_enabled() && std::uint64_t(w)*h >= 1280u*720u){
    Plane out;
    if(try_native_gaussian_cuda(p,w,h,sigma,out))return out;
  }
  int r = std::clamp(int(std::nearbyint(2.5f * sigma)), 1, 32);
  Plane k(2 * r + 1);
  float total = 0;
  for (int i = -r; i <= r; ++i) {
    k[i + r] = std::exp(-i * i / (2 * sigma * sigma));
    total += k[i + r];
  }
  for (auto &x : k)
    x /= total;

  thread_local Plane tl_tmp;
  if (tl_tmp.size() < p.size()) tl_tmp.resize(p.size());
  Plane* tmp_buf = &tl_tmp;

  parallel_rows(w, h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      const float* in_row = &p[y * w];
      float* tmp_row = &(*tmp_buf)[y * w];
      int left_end = std::min(r, w);
      for (int x = 0; x < left_end; ++x) {
        float v = 0;
        for (int i = -r; i <= r; ++i)
          v += in_row[reflect(x + i, w)] * k[i + r];
        tmp_row[x] = v;
      }
      int mid_end = std::max(r, w - r);
      for (int x = r; x < mid_end; ++x) {
        float v = 0;
        const float* src = in_row + x - r;
        for (int i = 0; i <= 2 * r; ++i)
          v += src[i] * k[i];
        tmp_row[x] = v;
      }
      for (int x = mid_end; x < w; ++x) {
        float v = 0;
        for (int i = -r; i <= r; ++i)
          v += in_row[reflect(x + i, w)] * k[i + r];
        tmp_row[x] = v;
      }
    }
  });

  Plane out(p.size());
  parallel_rows(w, h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      float* out_row = &out[y * w];
      for (int x = 0; x < w; ++x) out_row[x] = 0.0f;
      for (int i = -r; i <= r; ++i) {
        int sy = reflect(y + i, h);
        const float* in_row = &(*tmp_buf)[sy * w];
        const float weight = k[i + r];
        for (int x = 0; x < w; ++x) {
          out_row[x] += in_row[x] * weight;
        }
      }
    }
  });
  return out;
}
Plane morph(const Plane &p, int w, int h, int radius, bool maximum) {
  if(native_cuda_enabled() && std::uint64_t(w)*h >= 1280u*720u){
    Plane out;
    if(try_native_morph_cuda(p,w,h,radius,maximum,out))return out;
  }
  Plane tmp(p.size()), out(p.size());
  auto combine = [&](float a, float b) {
    return maximum ? std::max(a, b) : std::min(a, b);
  };
  parallel_rows(w,h,[&](int y0,int y1){
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < w; ++x) {
        float v = p[y * w + x];
        for (int i = -radius; i <= radius; ++i)
          v = combine(v, p[y * w + std::clamp(x + i, 0, w - 1)]);
        tmp[y * w + x] = v;
      }
  });
  parallel_rows(w,h,[&](int y0,int y1){
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < w; ++x) {
        float v = tmp[y * w + x];
        for (int i = -radius; i <= radius; ++i)
          v = combine(v, tmp[std::clamp(y + i, 0, h - 1) * w + x]);
        out[y * w + x] = v;
      }
  });
  return out;
}
Plane average_zero(const Plane &p, int w, int h, int r) {
  const std::size_t int_size = std::size_t(w + 1) * (h + 1);
  thread_local std::vector<double> tl_integral;
  if (tl_integral.size() < int_size) tl_integral.resize(int_size);
  auto* int_ptr = &tl_integral;
  
  for (int y = 0; y < h; ++y) {
    double row = 0;
    for (int x = 0; x < w; ++x) {
      row += p[y * w + x];
      (*int_ptr)[(y + 1) * (w + 1) + x + 1] = (*int_ptr)[y * (w + 1) + x + 1] + row;
    }
  }
  Plane out(p.size());
  const double inv_area = 1.0 / (double((2 * r + 1) * (2 * r + 1)));
  parallel_rows(w,h,[&](int yy0,int yy1){
    for (int y = yy0; y < yy1; ++y)
      for (int x = 0; x < w; ++x) {
        int x0 = std::max(0, x - r), y0 = std::max(0, y - r),
            x1 = std::min(w, x + r + 1), y1 = std::min(h, y + r + 1);
        out[y * w + x] =
            float(((*int_ptr)[y1 * (w + 1) + x1] - (*int_ptr)[y0 * (w + 1) + x1] -
                   (*int_ptr)[y1 * (w + 1) + x0] + (*int_ptr)[y0 * (w + 1) + x0]) *
                  inv_area);
      }
  });
  return out;
}
Plane luma(const RGB &rgb) {
  Plane p(rgb[0].size());
  for (std::size_t i = 0; i < p.size(); ++i)
    p[i] = .299f * rgb[0][i] + .587f * rgb[1][i] + .114f * rgb[2][i];
  return p;
}
float sigmoid(float x) { return 1 / (1 + std::exp(-x)); }
float bilinear(const Plane &p, int w, int h, float x, float y) {
  x = std::clamp(x, 0.f, float(w - 1));
  y = std::clamp(y, 0.f, float(h - 1));
  int ix = int(x), iy = int(y);
  float dx = x - ix, dy = y - iy;
  int xx = std::min(ix + 1, w - 1), yy = std::min(iy + 1, h - 1);
  return (p[iy * w + ix] * (1 - dx) + p[iy * w + xx] * dx) * (1 - dy) +
         (p[yy * w + ix] * (1 - dx) + p[yy * w + xx] * dx) * dy;
}
float param(const EffectInstance &e, const char *key, double t, float fallback,
            float lo, float hi) {
  double v = effect_numeric(e, key, t, fallback);
  return std::isfinite(v) ? std::clamp(float(v), lo, hi) : fallback;
}
struct Detector {
  Plane smooth, valley, gx, gy, gradient, blackhat, zone;
};
Detector detect(const RGB &rgb, int w, int h, float denoise, float local,
                int blackhat) {
  Detector d;
  d.smooth = gaussian(luma(rgb), w, h, denoise);
  d.valley = gaussian(d.smooth, w, h, local);
  auto closed = morph(morph(d.smooth, w, h, blackhat / 2, true), w, h,
                      blackhat / 2, false);
  auto n = d.smooth.size();
  d.gx.resize(n);
  d.gy.resize(n);
  d.gradient.resize(n);
  d.blackhat.resize(n);
  auto at = [&](int x, int y) {
    return d.smooth[reflect(y, h) * w + reflect(x, w)];
  };
  parallel_rows(w,h,[&](int y0,int y1){
    for (int y = y0; y < y1; ++y) {
      if (y > 0 && y < h - 1) {
        const float* r_prev = &d.smooth[(y - 1) * w];
        const float* r_curr = &d.smooth[y * w];
        const float* r_next = &d.smooth[(y + 1) * w];
        
        // Left boundary
        {
          int x = 0;
          int i = y * w + x;
          float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1)) * 0.125f;
          float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1)) * 0.125f;
          d.gx[i] = gx;
          d.gy[i] = gy;
          d.gradient[i] = std::sqrt(gx * gx + gy * gy + 1e-7f);
          d.valley[i] = std::max(0.f, d.valley[i] - d.smooth[i]);
          d.blackhat[i] = std::max(0.f, closed[i] - d.smooth[i]);
        }
        
        // Fast interior without reflect
        for (int x = 1; x < w - 1; ++x) {
          int i = y * w + x;
          float gx = (r_prev[x + 1] + 2.f * r_curr[x + 1] + r_next[x + 1] -
                      r_prev[x - 1] - 2.f * r_curr[x - 1] - r_next[x - 1]) * 0.125f;
          float gy = (r_next[x - 1] + 2.f * r_next[x] + r_next[x + 1] -
                      r_prev[x - 1] - 2.f * r_prev[x] - r_prev[x + 1]) * 0.125f;
          d.gx[i] = gx;
          d.gy[i] = gy;
          d.gradient[i] = std::sqrt(gx * gx + gy * gy + 1e-7f);
          d.valley[i] = std::max(0.f, d.valley[i] - d.smooth[i]);
          d.blackhat[i] = std::max(0.f, closed[i] - d.smooth[i]);
        }
        
        // Right boundary
        if (w > 1) {
          int x = w - 1;
          int i = y * w + x;
          float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1)) * 0.125f;
          float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1)) * 0.125f;
          d.gx[i] = gx;
          d.gy[i] = gy;
          d.gradient[i] = std::sqrt(gx * gx + gy * gy + 1e-7f);
          d.valley[i] = std::max(0.f, d.valley[i] - d.smooth[i]);
          d.blackhat[i] = std::max(0.f, closed[i] - d.smooth[i]);
        }
      } else {
        // Boundary rows y == 0 or y == h - 1
        for (int x = 0; x < w; ++x) {
          int i = y * w + x;
          float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1)) * 0.125f;
          float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) -
                      at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1)) * 0.125f;
          d.gx[i] = gx;
          d.gy[i] = gy;
          d.gradient[i] = std::sqrt(gx * gx + gy * gy + 1e-7f);
          d.valley[i] = std::max(0.f, d.valley[i] - d.smooth[i]);
          d.blackhat[i] = std::max(0.f, closed[i] - d.smooth[i]);
        }
      }
    }
  });
  return d;
}
Plane text_zone(const Detector &d, int w, int h, float local) {
  Plane edges(d.gradient.size());
  for (std::size_t i = 0; i < edges.size(); ++i)
    edges[i] = d.gradient[i] > .4f ? 1 : 0;
  auto density = average_zero(edges, w, h, 10);
  for (auto &x : density)
    x = sigmoid(20 * (x - .15f));
  return gaussian(density, w, h, local);
}
RGB vector_color(const RGB &original, int w, int h, const EffectInstance &e,
                 double t) {
  RGB out = original;
  float flat = param(e, "flatness", t, .65f, 0, 1),
        levels = param(e, "levels", t, 48, 0, 256),
        contrast = param(e, "contrast", t, 1.1f, 0, 3),
        sat = param(e, "saturation", t, 1.25f, 0, 3),
        blend = param(e, "original_blend", t, .35f, 0, 1);
  if (flat > 0)
    for (int c = 0; c < 3; ++c) {
      auto smoothed = gaussian(original[c], w, h, 2.5f);
      for (std::size_t i = 0; i < out[c].size(); ++i)
        out[c][i] =
            levels > 0
                ? smoothed[i] + (std::nearbyint(smoothed[i] * levels) / levels -
                                 smoothed[i]) *
                                    flat
                : original[c][i] + (smoothed[i] - original[c][i]) * flat;
    }
  for (std::size_t i = 0; i < out[0].size(); ++i) {
    for (int c = 0; c < 3; ++c)
      out[c][i] = clamp((out[c][i] - .5f) * contrast + .5f);
    float lum = .299f * out[0][i] + .587f * out[1][i] + .114f * out[2][i];
    for (int c = 0; c < 3; ++c)
      out[c][i] = clamp(lum + (out[c][i] - lum) * sat) * (1 - blend) +
                  original[c][i] * blend;
  }
  return out;
}
void restore_lines(RGB &out, const RGB &original, int w, int h,
                   const EffectInstance &e, double t, bool text) {
  float local = param(e, "local_sigma", t, 3, .1f, 10);
  int size = int(param(e, "blackhat_size", t, 7, 3, 21)) | 1;
  auto d = detect(original, w, h, param(e, "denoise_sigma", t, .8f, .1f, 5),
                  local, size);
  float threshold = param(e, "threshold", t, 16, 0, 80),
        maxlum = param(e, "max_luma", t, 230, 0, 255) / 255,
        artifact = param(e, "artifact_threshold", t, .08f, 0, 1);
  Plane candidate(d.smooth.size()), ridge(candidate.size());
  for (std::size_t i = 0; i < candidate.size(); ++i)
    candidate[i] =
        (.55f * d.blackhat[i] + .30f * d.valley[i] + .15f * d.gradient[i]) *
                        255 >=
                    threshold &&
                d.smooth[i] <= maxlum
            ? 1
            : 0;
  auto d3 = average_zero(candidate, w, h, 1),
       d7 = average_zero(candidate, w, h, 3);
  if (text)
    d.zone = text_zone(d, w, h, local);
  parallel_rows(w, h, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < w; ++x) {
        int i = y * w + x;
        if (!candidate[i] || d3[i] < .22f || d7[i] < artifact ||
            (text && d.zone[i] >= .2f))
          continue;
        float nx = d.gx[i] / (d.gradient[i] + 1e-6f) * (w - 1) / w,
              ny = d.gy[i] / (d.gradient[i] + 1e-6f) * (h - 1) / h;
        ridge[i] = d.valley[i] >= bilinear(d.valley, w, h, x + nx, y + ny) &&
                           d.valley[i] >= bilinear(d.valley, w, h, x - nx, y - ny)
                       ? 1
                       : 0;
      }
    }
  });
  auto stroke =
           gaussian(ridge, w, h, param(e, "line_width", t, 1.1f, .1f, 5) * .5f),
       maximum = morph(stroke, w, h, 1, true);
  float hard = param(e, "edge_hardness", t, 8, 1, 20),
        minimum = sigmoid(-.4f * hard),
        strength = param(e, "strength", t, .85f, 0, 1.5f),
        peak = *std::max_element(d.blackhat.begin(), d.blackhat.end());
  Plane alpha(stroke.size()), weight(alpha.size());
  for (std::size_t i = 0; i < alpha.size(); ++i) {
    alpha[i] = clamp(
        (sigmoid(hard * (stroke[i] / (maximum[i] + 1e-6f) - .4f)) - minimum) /
        (1 - minimum) * strength *
        std::clamp(d.blackhat[i] / (peak + 1e-6f), .2f, 1.f));
    weight[i] = alpha[i] + 1e-4f;
  }
  auto denom = gaussian(weight, w, h, local);
  float gamma = 1 + param(e, "color_pull", t, .85f, 0, 1) * 1.5f;
  for (int c = 0; c < 3; ++c) {
    Plane weighted(alpha.size());
    for (std::size_t i = 0; i < alpha.size(); ++i)
      weighted[i] = original[c][i] * weight[i];
    weighted = gaussian(weighted, w, h, local);
    for (std::size_t i = 0; i < alpha.size(); ++i) {
      float ink = std::pow(
          std::clamp(weighted[i] / std::max(denom[i], 1e-6f), 1e-4f, 1.f),
          gamma);
      out[c][i] = out[c][i] * (1 - alpha[i]) + ink * alpha[i];
    }
  }
}
void restore_text(RGB &out, const RGB &original, int w, int h,
                  const EffectInstance &e, double t, bool full) {
  float local = param(e, "local_sigma", t, 3, .1f, 10);
  auto d = detect(original, w, h, param(e, "denoise_sigma", t, .8f, .1f, 5),
                  local, 7);
  auto zone = text_zone(d, w, h, local);
  auto lum = luma(original), bg = gaussian(lum, w, h, 2.5f);
  float threshold =
            param(e, full ? "text_threshold" : "threshold", t, .1f, 0, 1),
        boost = param(e, full ? "text_boost" : "boost", t, .2f, 0, 1);
  for (int c = 0; c < 3; ++c) {
    auto clean = gaussian(morph(morph(out[c], w, h, 3, false), w, h, 3, true),
                          w, h, local);
    for (std::size_t i = 0; i < lum.size(); ++i) {
      float a = sigmoid(30 * (std::max(0.f, lum[i] - bg[i]) - threshold));
      float restored =
          clamp(original[c][i] * (1 + boost)) * a + clean[i] * (1 - a);
      out[c][i] += zone[i] * (restored - out[c][i]);
    }
  }
}
void paper(RGB &out, int w, int h, const EffectInstance &e, double t,
           bool full) {
  float texture = param(e, full ? "paper_texture" : "texture", t, .05f, 0, .5f),
        warmth = param(e, full ? "paper_warmth" : "warmth", t, .08f, 0, .5f);
  const float scale=param(e,"paper_scale",t,1,.25f,4),
      contrast=param(e,"paper_contrast",t,1,0,3),coarse=param(e,"paper_coarse",t,.65f,0,1),
      fibers=param(e,"paper_fibers",t,0,0,1),stains=param(e,"paper_stains",t,0,0,1),
      aging=param(e,"paper_aging",t,0,0,1),shadows=param(e,"paper_shadows",t,0,0,1),
      protect=param(e,"paper_protect",t,0,0,1),motion=param(e,"paper_motion",t,0,0,24);
  if(texture==0&&warmth==0&&aging==0&&fibers==0&&stains==0)return;
  Plane noise(out[0].size());
  std::uint32_t state = 0x6d2b79f5u ^ (std::uint32_t(param(e,"paper_seed",t,0,0,10000))*0x9e3779b9u) ^ (std::uint32_t(std::fmod(std::floor(std::max(0.0,t)*motion),4294967295.0))*0x85ebca6bu);
  for (auto &v : noise) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    v = float(state & 65535) / 32767.5f - 1;
  }
  auto low = gaussian(noise, w, h, 6*scale), high = gaussian(noise, w, h, 1.2f*scale);
  auto normalize = [](Plane &p) {
    double mean = 0, var = 0;
    for (float x : p)
      mean += x;
    mean /= p.size();
    for (float x : p)
      var += (x - mean) * (x - mean);
    float sd = std::sqrt(var / std::max<std::size_t>(1, p.size() - 1)) + 1e-5f;
    for (auto &x : p)
      x = (x - float(mean)) / sd;
  };
  normalize(low);
  normalize(high);
  auto lum = luma(out);
  auto blotches=stains>0?gaussian(noise,w,h,12*scale):Plane{};
  if(!blotches.empty())normalize(blotches);
  const float angle=param(e,"paper_angle",t,0,-180,180)*3.14159265f/180;
  const float tint[3]={1+warmth*.5f+aging*.10f,1+warmth*.1f-aging*.04f,1-warmth*.4f-aging*.22f};
  parallel_rows(w,h,[&](int y0,int y1){
  for(int y=y0;y<y1;++y)for(int x=0;x<w;++x){
    const std::size_t i=std::size_t(y)*w+x;
    const float edge=std::abs(lum[i]-lum[std::size_t(y)*w+std::min(x+1,w-1)])+
                     std::abs(lum[i]-lum[std::size_t(std::min(y+1,h-1))*w+x]);
    const float mask=(shadows+(1-shadows)*clamp(lum[i]))*(1-protect*clamp(edge*8));
    const float fiber=std::sin((x*std::cos(angle)+y*std::sin(angle))/scale*1.7f+low[i]*.25f)*fibers*.07f;
    const float blot=blotches.empty()?0:blotches[i]*stains*.06f;
    const float grain=(coarse*low[i]+(1-coarse)*high[i])*texture*contrast+fiber+blot;
    for(int c=0;c<3;++c)out[c][i]*=1+((1+grain)*tint[c]-1)*mask;
  }
  });
}
void sharpen(RGB &out, int w, int h, float amount) {
  if (amount <= 0)
    return;
  for (auto &p : out) {
    auto blur = gaussian(p, w, h, 1.2f);
    for (std::size_t i = 0; i < p.size(); ++i)
      p[i] += amount * (p[i] - blur[i]);
  }
}
} // namespace
bool is_native_exact_effect(const EffectInstance &e) {
  if (!e.enabled)
    return false;
  const std::string &id = e.definition_id;
  return id == "koralo.anime_restore_v45" ||
         id == "koralo.anime_line_restore" ||
         id == "koralo.anime_vector_color" ||
         id == "koralo.anime_text_restore" ||
         id == "koralo.anime_paper_texture" ||
         id == "koralo.anime_detail_sharpen" ||
         id == "koralo.anime_deinterlace" || id == "koralo.layer_mask" ||
         id == "koralo.paint_mask" || id == "koralo.curves";
}
bool apply_native_effect(Frame &f, const EffectInstance &e, double t) {
  if (!is_native_exact_effect(e))
    return false;
  int w = f.width, h = f.height;
  std::size_t n = std::size_t(std::max(0, w)) * std::max(0, h);
  if (!n || f.rgb.size() != n * 3)
    return true;
  const auto &id = e.definition_id;
  if (id == "koralo.layer_mask") {
    float cx = param(e, "center_x", t, .5f, 0, 1),
          cy = param(e, "center_y", t, .5f, 0, 1),
          sx = param(e, "width", t, .8f, .001f, 2),
          sy = param(e, "height", t, .8f, .001f, 2),
          feather = param(e, "feather", t, .02f, 0, .5f);
    bool ellipse = effect_numeric(e, "shape", t, 1) > .5,
         invert = effect_numeric(e, "invert", t, 0) > .5;
    if (f.alpha.size() != n)
      f.alpha.assign(n, 255);
    parallel_rows(w,h,[&](int y0,int y1){
    for (int y = y0; y < y1; ++y)
      for (int x = 0; x < w; ++x) {
        float dx = std::abs((x + .5f) / w - cx),
              dy = std::abs((y + .5f) / h - cy),
              distance = ellipse
                             ? (1 - std::sqrt(std::pow(dx / (sx * .5f), 2) +
                                              std::pow(dy / (sy * .5f), 2))) *
                                   std::min(sx, sy) * .5f
                             : std::min(sx * .5f - dx, sy * .5f - dy);
        float a = feather > 0 ? clamp(distance / feather + .5f)
                              : (distance >= 0 ? 1 : 0);
        a = a * a * (3 - 2 * a);
        if (invert)
          a = 1 - a;
        f.alpha[y * w + x] = std::uint8_t(std::lround(f.alpha[y * w + x] * a));
      }
    });
    return true;
  }
  // A compact vector stroke mask: each line is x0 y0 x1 y1 radius hardness
  // erase. Normalized coordinates survive project resolution and preview
  // quality changes.
  if (id == "koralo.paint_mask") {
    const auto* state=effect_parameter(e,"strokes");
    const auto* text=state?std::get_if<std::string>(&state->value):nullptr;
    if(text)apply_paint_mask(f,*text,e.id);
    return true;
  }
  RGB original;
  for (auto &p : original)
    p.resize(n);
  for (std::size_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c)
      original[c][i] = f.rgb[i * 3 + c] / 255.f;
  RGB out = original;
  bool full = id == "koralo.anime_restore_v45";
  if (full || id == "koralo.anime_vector_color")
    out = vector_color(original, w, h, e, t);
  if (full || id == "koralo.anime_line_restore")
    restore_lines(out, original, w, h, e, t,
                  full && effect_numeric(e, "restore_text", t, 0) > .5);
  if ((full && effect_numeric(e, "restore_text", t, 0) > .5) ||
      id == "koralo.anime_text_restore")
    restore_text(out, original, w, h, e, t, full);
  if (full || id == "koralo.anime_paper_texture")
    paper(out, w, h, e, t, full);
  if (full || id == "koralo.anime_detail_sharpen")
    sharpen(out, w, h,
            param(e, full ? "post_sharpen" : "amount", t, .5f, 0, 2));
  if (id == "koralo.anime_deinterlace") {
    float strength = param(e, "strength", t, 1, 0, 1);
    for (int c = 0; c < 3; ++c)
      for (int y = 1; y < h - 1; y += 2)
        for (int x = 0; x < w; ++x) {
          int i = y * w + x;
          out[c][i] +=
              (.5f * (original[c][i - w] + original[c][i + w]) - out[c][i]) *
              strength;
        }
  }
  if (id == "koralo.curves") {
    const char *keys[] = {"black", "shadows", "midtones", "highlights",
                          "white"};
    float points[5];
    for (int i = 0; i < 5; ++i)
      points[i] = param(e, keys[i], t, i * .25f, 0, 1);
    int channel = int(param(e, "channel", t, 0, 0, 3));
    for (int c = 0; c < 3; ++c)
      if (channel == 0 || channel == c + 1)
        for (auto &v : out[c]) {
          float x = clamp(v) * 4;
          int i = std::min(3, int(x));
          v = points[i] + (points[i + 1] - points[i]) * (x - i);
        }
  }
  for (std::size_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c)
      f.rgb[i * 3 + c] = std::uint8_t(clamp(out[c][i]) * 255.f);
  return true;
}
} // namespace koralo
