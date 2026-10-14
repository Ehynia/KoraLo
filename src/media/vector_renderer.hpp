#pragma once
#include "../core/vector_animation.hpp"
namespace koralo {
struct Frame;
Frame render_vector_scene(const VectorScene &, int width, int height,
                          double time);
} // namespace koralo
