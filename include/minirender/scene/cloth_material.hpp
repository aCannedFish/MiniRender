#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace minirender {

// 一条纱线的切向角曲线。每段在两个角之间线性变化，lengths 给出相对采样长度。
// 零长度段表示方向跳变，不分配采样；角度相对宏观切平面，以度为单位。
struct ClothTangentCurve {
    std::vector<double> angles{-25, 25};
    std::vector<double> lengths{1};
};

// Sadeghi 2013 的表面与内部散射参数。albedo 为内部散射的线性 RGB，kd 为各向同性比例。
struct ClothThread {
    glm::dvec3 albedo{.06, .24, .3};
    double kd = .3;
    double surface_width_degrees = 12;
    double volume_width_degrees = 24;
    ClothTangentCurve curve;
};

struct ClothAppearanceSettings {
    std::array<ClothThread, 2> threads;
    std::array<double, 2> coverage{.33, .33};
    std::uint32_t samples_per_thread = 8;
    double rotation_degrees = 0;
    double correlation_width_degrees = 20;
    void validate() const;
};

// 按曲线长度等距中点求积。weight 包含纱线覆盖率，用于论文的加权求和及归一项。
struct ClothTangentSample {
    double cosine = 1;
    double sine = 0;
    double weight = 0;
    std::uint32_t thread = 0;
};

// 不可变的材质参数和已编译曲线，可由场景快照、CPU BSDF 和 GPU 打包共同引用。
class ClothAppearance final {
  public:
    explicit ClothAppearance(ClothAppearanceSettings settings = {});
    const ClothAppearanceSettings& settings() const noexcept;
    std::span<const ClothTangentSample> samples() const noexcept;
    double coverage() const noexcept;

  private:
    ClothAppearanceSettings settings_;
    std::vector<ClothTangentSample> samples_;
};

struct ClothPreset {
    std::string name;
    double ior = 1.46;
    std::shared_ptr<const ClothAppearance> appearance;
};

// 取自 Sadeghi 2013 Table II：linen、silk、satin、shot，保留原文颜色和角度单位。
ClothPreset make_cloth_preset(const std::string& name);

} // namespace minirender
