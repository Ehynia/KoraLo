#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace koralo {
enum class KeyframeInterpolation { Linear, Hold, Smooth };
struct Keyframe {
  double time = 0, value = 0;
  KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
  // Outgoing cubic Bezier easing. X handles are monotonic in [0,1].
  double x1 = .33, y1 = 0, x2 = .67, y2 = 1;
};
struct AnimatedDouble {
  double default_value = 0;
  std::vector<Keyframe> keys;
  double value_at(double time) const;
  void set_key(double time, double value,
               KeyframeInterpolation mode = KeyframeInterpolation::Linear);
  bool erase_key(double time, double epsilon = 1e-6);
  bool has_key(double time, double epsilon = 1e-6) const;
};
struct VectorPoint {
  double x = 0, y = 0;
};
struct VectorNode {
  VectorPoint point, in, out; // Tangents are offsets from the anchor.
};
struct VectorColor {
  double r = 1, g = 1, b = 1, a = 1;
};
struct VectorShape {
  int id = 0;
  std::string name = "Контур";
  bool visible = true, locked = false, closed = true, fill_enabled = true;
  std::vector<VectorNode> nodes;
  VectorColor fill{1, 1, 1, 1}, stroke{.3, .81, .75, 1};
  double stroke_width = .003;
  AnimatedDouble x{.5, {}}, y{.5, {}}, scale{1, {}}, rotation{0, {}},
      opacity{1, {}};
};
struct VectorScene {
  bool enabled = false;
  std::vector<VectorShape> shapes; // Back to front.
  std::string mask_strokes;
  AnimatedDouble mask_amount{1, {}};
  bool crop_enabled = false;
  AnimatedDouble crop_x{0, {}}, crop_y{0, {}}, crop_w{1, {}}, crop_h{1, {}};
  int next_shape_id = 1;
  VectorShape *shape(int id);
  const VectorShape *shape(int id) const;
};
VectorShape make_vector_rectangle(int id);
VectorShape make_vector_ellipse(int id);
VectorShape make_vector_ribbon(int id);
VectorPoint transform_vector_point(const VectorShape &, VectorPoint,
                                   double time, double aspect = 1);
VectorPoint inverse_vector_point(const VectorShape &, VectorPoint, double time,
                                 double aspect = 1);
std::vector<VectorPoint> flatten_vector_path(const VectorShape &, double time,
                                             int subdivisions = 16,
                                             double aspect = 1);
std::string encode_vector_scene(const VectorScene &);
bool decode_vector_scene(const std::string &, VectorScene &,
                         std::string *error = nullptr);
std::uint64_t vector_scene_fingerprint(const VectorScene &);
AnimatedDouble *vector_channel(VectorShape &, int property);
AnimatedDouble *scene_channel(VectorScene &, int property);
double keyframe_easing(const Keyframe &, double fraction);
} // namespace koralo
