#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace koralo {

enum class EffectParameterType { Double, Integer, Bool, Choice, ColorRGBA, Vec2, String, Group, Action };
enum class EffectKeyInterpolation { Linear, Hold };
enum EffectContext : std::uint32_t { EffectMedia=1u, EffectEvent=2u, EffectTrack=4u, EffectOutput=8u };

struct EffectColor { double r=0.0,g=0.0,b=0.0,a=1.0; bool operator==(const EffectColor&) const = default; };
struct EffectVec2 { double x=0.0,y=0.0; bool operator==(const EffectVec2&) const = default; };
using EffectValue = std::variant<double,int,bool,std::string,EffectColor,EffectVec2>;

struct EffectKeyframe {
    double time=0.0;
    double value=0.0;
    EffectKeyInterpolation interpolation=EffectKeyInterpolation::Linear;
};

struct ParameterDefinition {
    std::string id;
    std::string name;
    EffectParameterType type=EffectParameterType::Double;
    EffectValue default_value=0.0;
    double min_value=0.0;
    double max_value=1.0;
    double step=0.1;
    bool animatable=false;
    std::vector<std::string> choices;
};

struct EffectDefinition {
    std::string id;
    std::string name;
    std::string category;
    std::uint32_t contexts=EffectEvent|EffectTrack|EffectOutput;
    bool requires_analysis=false;
    std::vector<ParameterDefinition> parameters;
};

struct EffectParameterState {
    std::string id;
    EffectValue value=0.0;
    std::vector<EffectKeyframe> keys;
    bool animated=false;

    [[nodiscard]] double numeric_value(double time, double fallback=0.0) const;
    void set_numeric_key(double time,double v,EffectKeyInterpolation interpolation=EffectKeyInterpolation::Linear);
};

struct EffectInstance {
    int id=0;
    std::string definition_id;
    bool enabled=true;
    std::vector<EffectParameterState> parameters;
};

const std::vector<EffectDefinition>& builtin_effect_definitions();
const std::vector<EffectDefinition>& builtin_audio_effect_definitions();
const std::vector<EffectDefinition>& external_effect_definitions();
void set_external_effect_definitions(std::vector<EffectDefinition> definitions);
std::vector<const EffectDefinition*> available_effect_definitions();
std::vector<const EffectDefinition*> available_audio_effect_definitions();
int effect_preview_cost(const std::vector<EffectInstance>& effects);
const EffectDefinition* effect_definition(const std::string& id);
const EffectDefinition* audio_effect_definition(const std::string& id);
EffectInstance make_effect_instance(const std::string& definition_id,int instance_id=0);
EffectInstance make_audio_effect_instance(const std::string& definition_id,int instance_id=0);
EffectParameterState* effect_parameter(EffectInstance& instance,const std::string& id);
const EffectParameterState* effect_parameter(const EffectInstance& instance,const std::string& id);
double effect_numeric(const EffectInstance& instance,const std::string& id,double time,double fallback=0.0);
bool set_effect_numeric(EffectInstance& instance,const std::string& id,double value);
std::string effect_value_string(const EffectValue& value);

} // namespace koralo
