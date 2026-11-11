#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace minirender {

// 均匀高散射材质的光学参数，系数单位为 m^-1。约化散射已包含 (1-g)。
// density 同时缩放吸收和散射，改变扩散距离而保持约化反照率。
struct SubsurfaceParameters {
    glm::dvec3 absorption{2.1, 4.1, 7.1};
    glm::dvec3 reduced_scattering{2190, 2620, 3000};
    double density = 1;
    void validate() const;
    bool operator==(const SubsurfaceParameters&) const = default;
};

struct SubsurfacePreset {
    std::string id;
    double ior;
    std::shared_ptr<const SubsurfaceParameters> parameters;
};

// marble 使用 Jensen 2001 的测量系数；wax 使用项目自定的暖色展示参数。
SubsurfacePreset make_subsurface_preset(const std::string& name);

} // namespace minirender
