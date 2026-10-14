#pragma once
#include "../core/effects.hpp"
namespace koralo {
struct Frame;
bool is_native_exact_effect(const EffectInstance &effect);
bool apply_native_effect(Frame &frame, const EffectInstance &effect,
                         double time);
} // namespace koralo
