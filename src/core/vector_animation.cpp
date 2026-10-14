#include "vector_animation.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace koralo {
namespace {
double cubic(double a, double b, double t) {
  const double u = 1 - t;
  return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}
void write_channel(std::ostream &out, const AnimatedDouble &c) {
  out << ' ' << c.default_value << ' ' << c.keys.size();
  for (const auto &k : c.keys)
    out << ' ' << k.time << ' ' << k.value << ' ' << int(k.interpolation) << ' '
        << k.x1 << ' ' << k.y1 << ' ' << k.x2 << ' ' << k.y2;
}
bool read_number(std::istream &in, double &value, double lo = -10000,
                 double hi = 10000) {
  return bool(in >> value) && std::isfinite(value) && value >= lo &&
         value <= hi;
}
bool read_channel(std::istream &in, AnimatedDouble &c) {
  int n = 0;
  if (!read_number(in, c.default_value) || !(in >> n) || n < 0 || n > 2048)
    return false;
  double last = -1;
  for (int i = 0; i < n; ++i) {
    Keyframe k;
    int mode = 0;
    if (!read_number(in, k.time, 0, 43200) || k.time <= last ||
        !read_number(in, k.value) || !(in >> mode) || mode < 0 || mode > 2 ||
        !read_number(in, k.x1, 0, 1) || !read_number(in, k.y1, 0, 1) ||
        !read_number(in, k.x2, 0, 1) || !read_number(in, k.y2, 0, 1) ||
        k.x1 > k.x2)
      return false;
    k.interpolation = KeyframeInterpolation(mode);
    c.keys.push_back(k);
    last = k.time;
  }
  return true;
}
void write_color(std::ostream &out, const VectorColor &c) {
  out << ' ' << c.r << ' ' << c.g << ' ' << c.b << ' ' << c.a;
}
bool read_color(std::istream &in, VectorColor &c) {
  return read_number(in, c.r, 0, 1) && read_number(in, c.g, 0, 1) &&
         read_number(in, c.b, 0, 1) && read_number(in, c.a, 0, 1);
}
} // namespace
double keyframe_easing(const Keyframe &k, double fraction) {
  const double x = std::clamp(fraction, 0.0, 1.0);
  if (k.interpolation == KeyframeInterpolation::Hold)
    return x < 1 ? 0 : 1;
  if (k.interpolation != KeyframeInterpolation::Smooth)
    return x;
  double lo = 0, hi = 1;
  for (int i = 0; i < 24; ++i) {
    const double t = (lo + hi) * .5;
    if (cubic(k.x1, k.x2, t) < x)
      lo = t;
    else
      hi = t;
  }
  return cubic(k.y1, k.y2, (lo + hi) * .5);
}
VectorShape *VectorScene::shape(int id) {
  for (auto &s : shapes)
    if (s.id == id)
      return &s;
  return nullptr;
}
const VectorShape *VectorScene::shape(int id) const {
  for (const auto &s : shapes)
    if (s.id == id)
      return &s;
  return nullptr;
}
AnimatedDouble *vector_channel(VectorShape &s, int property) {
  switch (property) {
  case 0:
    return &s.x;
  case 1:
    return &s.y;
  case 2:
    return &s.scale;
  case 3:
    return &s.rotation;
  case 4:
    return &s.opacity;
  default:
    return nullptr;
  }
}
AnimatedDouble *scene_channel(VectorScene &s, int property) {
  switch (property) {
  case 5:
    return &s.mask_amount;
  case 6:
    return &s.crop_x;
  case 7:
    return &s.crop_y;
  case 8:
    return &s.crop_w;
  case 9:
    return &s.crop_h;
  default:
    return nullptr;
  }
}
VectorShape make_vector_rectangle(int id) {
  VectorShape s;
  s.id = id;
  s.name = "Прямоугольник";
  s.nodes = {{{-.18, -.14}, {}, {}},
             {{.18, -.14}, {}, {}},
             {{.18, .14}, {}, {}},
             {{-.18, .14}, {}, {}}};
  return s;
}
VectorShape make_vector_ellipse(int id) {
  VectorShape s;
  s.id = id;
  s.name = "Эллипс";
  s.fill = {.3, .81, .75, 1};
  constexpr double r = .15, k = .5522847498307936 * r;
  s.nodes = {{{0, -r}, {-k, 0}, {k, 0}},
             {{r, 0}, {0, -k}, {0, k}},
             {{0, r}, {k, 0}, {-k, 0}},
             {{-r, 0}, {0, k}, {0, -k}}};
  return s;
}
VectorShape make_vector_ribbon(int id) {
  VectorShape s;
  s.id = id;
  s.name = "Контур 01";
  s.nodes = {{{-.34, .03}, {0, .07}, {.10, -.19}},
             {{-.12, -.04}, {-.10, -.13}, {.13, .17}},
             {{.15, .05}, {-.12, .08}, {.13, -.18}},
             {{.34, -.10}, {-.06, -.06}, {0, .07}},
             {{.34, .02}, {0, -.03}, {-.08, -.07}},
             {{.15, .14}, {.10, -.12}, {-.14, .09}},
             {{-.12, .06}, {.13, .17}, {-.13, -.12}},
             {{-.34, .15}, {.08, -.17}, {0, -.05}}};
  return s;
}
VectorPoint transform_vector_point(const VectorShape &s, VectorPoint p,
                                   double t, double aspect) {
  const double a = s.rotation.value_at(t) * 3.141592653589793 / 180,
               z = std::clamp(s.scale.value_at(t), .01, 20.0);
  return {
      s.x.value_at(t) + z * (std::cos(a) * p.x - std::sin(a) * p.y / aspect),
      s.y.value_at(t) + z * (std::sin(a) * p.x * aspect + std::cos(a) * p.y)};
}
VectorPoint inverse_vector_point(const VectorShape &s, VectorPoint p,
                                 double t, double aspect) {
  const double a = -s.rotation.value_at(t) * 3.141592653589793 / 180,
               z = std::clamp(s.scale.value_at(t), .01, 20.0),
               x = (p.x - s.x.value_at(t)) / z, y = (p.y - s.y.value_at(t)) / z;
  return {std::cos(a) * x - std::sin(a) * y / aspect,
          std::sin(a) * x * aspect + std::cos(a) * y};
}
std::vector<VectorPoint> flatten_vector_path(const VectorShape &s, double time,
                                             int subdivisions, double aspect) {
  std::vector<VectorPoint> out;
  if (s.nodes.empty())
    return out;
  subdivisions = std::clamp(subdivisions, 1, 64);
  out.push_back(transform_vector_point(s, s.nodes.front().point, time, aspect));
  const auto count = s.closed ? s.nodes.size() : s.nodes.size() - 1;
  for (std::size_t i = 0; i < count; ++i) {
    const auto &a = s.nodes[i];
    const auto &b = s.nodes[(i + 1) % s.nodes.size()];
    for (int j = 1; j <= subdivisions; ++j) {
      double t = double(j) / subdivisions, u = 1 - t;
      VectorPoint p{
          u * u * u * a.point.x + 3 * u * u * t * (a.point.x + a.out.x) +
              3 * u * t * t * (b.point.x + b.in.x) + t * t * t * b.point.x,
          u * u * u * a.point.y + 3 * u * u * t * (a.point.y + a.out.y) +
              3 * u * t * t * (b.point.y + b.in.y) + t * t * t * b.point.y};
      out.push_back(transform_vector_point(s, p, time, aspect));
    }
  }
  return out;
}
std::string encode_vector_scene(const VectorScene &s) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << "V1 " << s.enabled << ' ' << s.next_shape_id
      << ' ' << s.crop_enabled << ' ' << std::quoted(s.mask_strokes);
  for (int p = 5; p < 10; ++p)
    write_channel(out, *scene_channel(const_cast<VectorScene &>(s), p));
  out << ' ' << s.shapes.size();
  for (const auto &shape : s.shapes) {
    out << ' ' << shape.id << ' ' << std::quoted(shape.name) << ' '
        << shape.visible << ' ' << shape.locked << ' ' << shape.closed << ' '
        << shape.fill_enabled << ' ' << shape.stroke_width;
    write_color(out, shape.fill);
    write_color(out, shape.stroke);
    for (int p = 0; p < 5; ++p)
      write_channel(out, *vector_channel(const_cast<VectorShape &>(shape), p));
    out << ' ' << shape.nodes.size();
    for (const auto &n : shape.nodes)
      out << ' ' << n.point.x << ' ' << n.point.y << ' ' << n.in.x << ' '
          << n.in.y << ' ' << n.out.x << ' ' << n.out.y;
  }
  return out.str();
}
bool decode_vector_scene(const std::string &text, VectorScene &target,
                         std::string *error) {
  auto fail = [&] {
    if (error)
      *error = "Invalid vector animation record";
    return false;
  };
  if (text.size() > 2 * 1024 * 1024)
    return fail();
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string tag;
  VectorScene s;
  int count = 0;
  if (!(in >> tag) || tag != "V1" ||
      !(in >> s.enabled >> s.next_shape_id >> s.crop_enabled >>
        std::quoted(s.mask_strokes)) ||
      s.mask_strokes.size() > 240000 || s.next_shape_id < 1)
    return fail();
  for (int p = 5; p < 10; ++p)
    if (!read_channel(in, *scene_channel(s, p)))
      return fail();
  if (!(in >> count) || count < 0 || count > 128)
    return fail();
  for (int i = 0; i < count; ++i) {
    VectorShape shape;
    int nodes = 0;
    if (!(in >> shape.id >> std::quoted(shape.name) >> shape.visible >>
          shape.locked >> shape.closed >> shape.fill_enabled) ||
        shape.id < 1 || shape.name.size() > 256 || s.shape(shape.id) ||
        !read_number(in, shape.stroke_width, 0, .2) ||
        !read_color(in, shape.fill) || !read_color(in, shape.stroke))
      return fail();
    for (int p = 0; p < 5; ++p)
      if (!read_channel(in, *vector_channel(shape, p)))
        return fail();
    if (!(in >> nodes) || nodes < 0 || nodes > 512)
      return fail();
    for (int j = 0; j < nodes; ++j) {
      VectorNode n;
      if (!read_number(in, n.point.x, -4, 4) ||
          !read_number(in, n.point.y, -4, 4) ||
          !read_number(in, n.in.x, -4, 4) || !read_number(in, n.in.y, -4, 4) ||
          !read_number(in, n.out.x, -4, 4) || !read_number(in, n.out.y, -4, 4))
        return fail();
      shape.nodes.push_back(n);
    }
    s.next_shape_id = std::max(s.next_shape_id, shape.id + 1);
    s.shapes.push_back(std::move(shape));
  }
  in >> std::ws;
  if (!in.eof())
    return fail();
  target = std::move(s);
  return true;
}
std::uint64_t vector_scene_fingerprint(const VectorScene &s) {
  const auto text = encode_vector_scene(s);
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}
} // namespace koralo
